# http-server-cpp

A production-shaped HTTP/1.1 server written from scratch in C++, on raw POSIX sockets and Linux epoll. No Boost.Asio, no libuv, no existing HTTP framework — the socket lifecycle, the protocol parser, the concurrency model, and the backpressure handling are all hand-written.

This isn't a toy that handles the happy path. Every claim below is backed by a live reproduction: the broken behavior was captured first, then the fix, then the fix was re-verified against the same test.

---

## What it does

- Accepts TCP connections and parses HTTP/1.1 manually — correct partial-read handling (a single `recv()` is never assumed to be a complete request), correct `Content-Length`-aware message boundaries, correct handling of multiple pipelined requests arriving in one read.
- Serves static files from a configurable directory with basic MIME-type detection.
- Returns correct status codes for malformed input: `400` (missing `Host` header, malformed request line), `411` (missing `Content-Length` on `POST`), `405` (unsupported method).
- Maintains HTTP/1.1 keep-alive connections through an explicit per-connection state machine (`Idle` / `Queued` / `InFlight` / `Draining`), so a connection mid-request or mid-response can never be mistaken for one that's genuinely idle.
- Processes pipelined requests in the correct response order, even when they arrive across separate `epoll` read events and race across different worker threads.
- Serves large files correctly under real backpressure (slow/throttled clients) without truncating the transfer or leaking the connection.
- Runs a bounded thread pool (12 workers, 1000-deep queue by default) that sheds load with a clean `503` instead of hanging or crashing when saturated.
- Degrades gracefully under file-descriptor pressure: a soft connection ceiling throttles new `accept()` calls before the OS ever refuses one, with the pressure logged directly at the source.
- Uses edge-triggered epoll (`EPOLLET`) correctly — reads and writes always drain to `EAGAIN`, never assuming one event means one complete operation.

## What it doesn't do

- No TLS/HTTPS. Everything is plaintext HTTP.
- No virtual hosting — one `public_dir`, one site.
- No dynamic routing beyond two hardcoded routes (`/`, `/about`); everything else is static file serving.
- No compression (gzip/br).
- No HTTP/2.
- No range requests (no resumable downloads, no video seeking).

This is a correctness- and concurrency-focused HTTP/1.1 engine, not a general-purpose production web server. If you want to actually host something with it, static content only, and put a real reverse proxy (nginx, Caddy) in front for TLS.

---

## Architecture

```
accept() ──► epoll (edge-triggered, single thread)
                │
                ▼
   bounded thread pool (12 workers, 1000-slot queue)
                │
                ▼
   per-connection sequence gate (strict FIFO write-back)
                │
                ▼
        try_write() ──► Complete / Pending / Failed
```

A single thread owns the `epoll` instance and never blocks on I/O. Every connection carries its own `std::mutex`, and every state transition happens under that lock — there is no global lock on the hot path. An independent sweep thread reaps genuinely idle, queued-too-long, or stalled-draining connections on separate timeouts, so a slow client downloading a large file is never confused with one that's actually gone idle.

### Connection state machine

| State | Meaning | Timeout |
|---|---|---|
| `Idle` | Fully processed, nothing in flight | 5s |
| `Queued` | Bytes received, awaiting a worker thread | 30s |
| `InFlight` | A worker thread is actively processing this request | never (exempt) |
| `Draining` | Response is writing but hit backpressure (`EAGAIN`) | 30s |

---

## Bugs found and fixed

Every one of these was reproduced broken before anything was changed, then reproduced fixed after.

### Process crash on malformed `Content-Length`

A `Content-Length` header with a non-numeric value (`Content-Length: abc`) threw an uncaught `std::invalid_argument` from `std::stoul` on a worker thread. An uncaught exception on a non-main thread calls `std::terminate()` — killing the entire process, not just the offending connection.

**Fixed** by wrapping the parse in a try/catch and validating that the parse consumed the entire value. Verified: the identical malformed request no longer crashes the process.

### Case-sensitive header lookup silently corrupting requests

The original `Content-Length` lookup was a manual, exact-case string search for `"Content-Length: "` — one specific capitalization, one specific space. A request sent with `content-length:` (lowercase) or `Content-Length:5` (no space) failed to match, the body length was read as zero, and the un-consumed body bytes corrupted the start of the next pipelined request on the same connection.

**Fixed** by reusing the existing case-insensitive header-lookup helper instead of a bespoke string search.

### Backpressured downloads killed mid-transfer

A large file to a throttled client hit `EAGAIN` mid-write (normal backpressure, not a failure) but was marked `Idle` regardless — indistinguishable from a connection that had genuinely gone idle. The timeout sweep reaped it at the 5-second mark, truncating the download.

**Fixed** by adding a dedicated `Draining` state with its own timeout, so the sweep can tell "still actively sending, just slow" apart from "actually idle." Verified live: a 10.5MB file at a 200KB/s throttle now completes byte-for-byte, hash-matched against the untruncated original.

### Pipelined responses returned out of order

Two requests pipelined on one connection could be picked up by two different worker threads and written back in whichever order finished first — a real RFC 7230 violation, not a cosmetic issue. This happened both within a single read event (multiple requests in one `recv()`) and across separate read events on the same connection.

**Fixed** with a per-connection sequence gate: a monotonic counter assigned at extraction time, and a write-side wait that enforces strict FIFO ordering even when the underlying batches genuinely race across threads. Reproduced broken, then reproduced fixed, on both variants of the bug.

### Uncontrolled file-descriptor exhaustion

Under a constrained file-descriptor limit and a connection flood, `accept()` began failing with no log signal at all — visible only indirectly, as a cascade of hung-up connections downstream.

**Fixed** with a soft connection ceiling derived from `getrlimit(RLIMIT_NOFILE)`: the accept loop proactively stops taking new connections and logs the pressure once it nears the real ceiling, instead of running blind until the OS refuses outright.

---

## Load testing

Benchmarked with `wrk` across three concurrency tiers to find out which resource actually gives first under load — not just to get one throughput number.

| Concurrency | Req/sec | p50 | p99 | Bottleneck |
|---|---|---|---|---|
| 100 | 33,172 | 2.71ms | 343.00ms | Work-queue contention |
| 1,000 | 20,282 | 47.59ms | 314.62ms | Work-queue contention |
| 10,000 | 23,679 | 136.00ms | 441.14ms | Work-queue contention + graceful 503 shedding |

CPU never exceeded roughly 54% utilization at any tier — the bounded work queue, not the CPU and not (at these levels) file descriptors, is the actual limiting resource. At 10,000 concurrent connections, 1,578 requests timed out and 40,649 received a clean `503` instead of hanging — the bounded queue degrading exactly as designed under genuine overload, rather than falling over.

A separate, deliberately constrained test (server restarted under `ulimit -n 200`) was used to force and confirm the fd-exhaustion failure mode directly, and to verify the fix: after the soft-ceiling patch, the same load produced clean `accept-throttled` log events instead of the previous silent cascade, with p99 latency improving from 20.51ms to 11.41ms at identical settings.

---

## Building

Requires CMake and a C++17-capable compiler.

```bash
git clone https://github.com/jj761/http-server-cpp.git
cd http-server-cpp
mkdir build && cd build
cmake ..
make
```

## Running

```bash
cd build
./http_server
```

Listens on port 8080 by default. Static files are served from `public/` at the project root — drop any HTML/CSS/JS/images there and they're reachable at the matching path.

```bash
curl http://localhost:8080/
```

## Testing it yourself

A few things worth trying against a running instance:

```bash
# Malformed request handling
curl -s -X FOOBAR -o /dev/null -w "http_code=%{http_code}\n" http://localhost:8080/
# expect 405

# Keep-alive + large file under throttle
curl --limit-rate 200k -o /tmp/test.bin http://localhost:8080/large_test.bin

# Connection flood
seq 1000 | xargs -P 200 -I{} curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/ | sort | uniq -c
```

## Project structure

```
src/
  main.cpp           entry point, thread pool + socket setup
  server.cpp         epoll event loop, accept handling, timeout sweep
  connection.cpp      per-connection state, backpressured writes, close funnel
  http_parsing.cpp    request-line parsing, header lookup, message extraction
  router.cpp          routing, malformed-request handling, static file serving
  thread_pool.cpp     bounded worker pool with condition-variable dispatch
include/
  *.hpp               corresponding headers
public/
  index.html          this project's own portfolio page, served by itself
```
