#include <iostream>
#include <string>
#include <cstring>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

struct RequestLine
{
    std::string method;
    std::string path;
};

// Consolidates what were previously two separate functions (parse_path,
// parse_method), each independently calling request.find(' ') on the
// same string. This was flagged in the Day 2/3 handover as Open Issue #2,
// deliberately deferred until the full set of needed request-line fields
// (method, path) was known. It now is, so this is the single
// consolidation point: one scan of the request line, both fields
// extracted together. Returns nullopt if the request line is malformed
// in any way (missing spaces, path not starting with '/').
std::optional<RequestLine> parse_request_line(const std::string &request)
{
    size_t first_space = request.find(' ');
    if (first_space == std::string::npos)
        return std::nullopt;

    size_t second_space = request.find(' ', first_space + 1);
    if (second_space == std::string::npos)
        return std::nullopt;

    std::string path = request.substr(first_space + 1, second_space - first_space - 1);
    if (path.empty() || path[0] != '/')
        return std::nullopt;

    RequestLine result;
    result.method = request.substr(0, first_space);
    result.path = path;
    return result;
}

// Checks for the presence of a header by name, case-insensitively,
// anchored to "\r\n" + name so it cannot false-match a longer header
// name that merely contains this one as a substring (e.g. X-Custom-Host
// must not match a search for "host"). This consolidates what were
// previously three independent lowercase-and-search implementations
// (has_host_header, has_content_length_header, and an inline check for
// "Connection: close" in main()), flagged in the Day 3 handover as Open
// Issue #3 once a third instance appeared. lowercase_name must already
// be lowercase and must NOT include the trailing colon; it is added here.
bool has_header(const std::string &request, const std::string &lowercase_name)
{
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = (header_end == std::string::npos)
                              ? request
                              : request.substr(0, header_end);

    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    return headers.find("\r\n" + lowercase_name + ":") != std::string::npos;
}

// Returns true if `path` ends with `suffix`. std::string::ends_with is
// C++20; this project is pinned to C++17 (CMakeLists.txt), so it is not
// available. This is a direct C++17 substitute, not a stylistic choice.
bool ends_with(const std::string &path, const std::string &suffix)
{
    if (suffix.size() > path.size())
        return false;
    return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Maps a request path's extension to a MIME type for the Content-Type
// header. Falls back to application/octet-stream for anything
// unrecognized, which is the correct default for unknown binary data
// (browsers will offer a download rather than attempt to render it,
// rather than guessing wrong and rendering garbage as text/html).
std::string get_mime_type(const std::string &path)
{
    if (ends_with(path, ".html"))
        return "text/html";
    if (ends_with(path, ".css"))
        return "text/css";
    if (ends_with(path, ".js"))
        return "application/javascript";
    if (ends_with(path, ".png"))
        return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg"))
        return "image/jpeg";
    return "application/octet-stream";
}

// Attempts to slice exactly one complete HTTP message out of `buffer`.
// Returns the message if one is fully present, std::nullopt otherwise.
// On success, `buffer` is mutated to contain only the bytes AFTER the
// sliced message (i.e. it becomes the leftover for the next call).
//
// This is the core of the keep-alive fix: it operates purely on data
// already in memory, performs no recv() calls, and is what makes the
// "next request's bytes already sitting in leftover" case work without
// touching the socket again.
std::optional<std::string> try_extract_message(std::string &buffer)
{
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos)
        return std::nullopt; // headers not fully received yet

    size_t content_length = 0;
    std::string cl_header = "Content-Length: ";
    size_t cl_pos = buffer.find(cl_header);
    if (cl_pos != std::string::npos && cl_pos < header_end)
    {
        size_t cl_end = buffer.find("\r\n", cl_pos);
        content_length = std::stoul(buffer.substr(cl_pos + cl_header.size(), cl_end - cl_pos - cl_header.size()));
    }

    size_t message_end = header_end + 4 + content_length; // 4 = strlen("\r\n\r\n")
    size_t body_received = buffer.size() - (header_end + 4);

    if (body_received < content_length)
        return std::nullopt; // body still incomplete

    std::string message = buffer.substr(0, message_end);
    buffer.erase(0, message_end); // leftover stays in buffer for the next call
    return message;
}

enum class ReadStatus
{
    Ok,           // a complete message was returned
    ClientClosed, // recv() returned 0, peer closed the connection
    TimedOut      // recv() returned EAGAIN/EWOULDBLOCK
};

// Reads exactly one complete HTTP message from client_fd.
// `leftover` is caller-owned, persists across calls for the lifetime of
// the connection, and holds any bytes read past the end of the current
// message (e.g. the start of a pipelined next request).
//
// Contract:
//   - On ReadStatus::Ok, the return string is a complete message and
//     `leftover` holds whatever (possibly empty) bytes came after it.
//   - On ClientClosed or TimedOut, the return string is empty and the
//     caller must close the connection. `leftover` is left as-is; it is
//     irrelevant once the connection is closing.
std::pair<std::string, ReadStatus> read_request(int client_fd, std::string &leftover)
{
    // Case 1: a full message is already sitting in leftover from the
    // previous call (pipelined request). No recv() needed at all.
    if (auto msg = try_extract_message(leftover))
        return {*msg, ReadStatus::Ok};

    char chunk[4096];

    while (true)
    {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);

        if (n == 0)
            return {"", ReadStatus::ClientClosed};

        if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                std::cerr << "read_request: timed out waiting for data\n";
            return {"", ReadStatus::TimedOut};
        }

        leftover.append(chunk, n);

        if (auto msg = try_extract_message(leftover))
            return {*msg, ReadStatus::Ok};

        // else: headers or body still incomplete, loop and recv() more
    }
}

// ---------------------------------------------------------------------
// ThreadPool
//
// Fixed-size worker pool with a bounded task queue. NEW as of Week 2 /
// Day 6. Design decisions (see handover doc for full reasoning, this is
// the short version):
//   - Fixed thread count, chosen at construction (main() passes
//     hardware_concurrency()). Not auto-scaling. This server's work is
//     I/O-bound (blocking on recv/send), so hardware_concurrency() is a
//     starting point to benchmark from, not a proven-optimal number.
//   - Bounded queue, not unbounded. An unbounded queue under sustained
//     overload just relocates the unbounded-growth problem thread-per-
//     connection had (too many threads) into "too many queued tasks
//     sitting in memory." A cap forces an explicit decision about what
//     happens when the server is overloaded, instead of letting memory
//     grow silently.
//   - try_submit() returns false on a full queue rather than blocking
//     the accept loop or throwing. main() uses this return value to
//     decide whether to send a 503 Service Unavailable before closing
//     the connection, rather than accepting unbounded backlog.
//   - Condition variable wait uses the standard lambda-predicate form
//     specifically to guard against spurious wakeups (a real, documented
//     hazard of cv.wait() with no predicate: the OS is permitted to wake
//     a waiting thread with no corresponding notify_*() call, so the
//     wait must always re-check its actual condition after waking, not
//     assume a wakeup implies the condition is true).
class ThreadPool
{
public:
    explicit ThreadPool(size_t thread_count, size_t max_queue_size)
        : max_queue_size_(max_queue_size), shutdown_(false)
    {
        // Guard against hardware_concurrency() returning 0, which the
        // standard permits when the value cannot be determined. A pool
        // of zero threads would silently accept tasks that are never
        // run, which is a much worse failure mode than just picking a
        // safe minimum.
        if (thread_count == 0)
            thread_count = 1;

        for (size_t i = 0; i < thread_count; ++i)
            workers_.emplace_back([this]
                                  { worker_loop(); });
    }

    // Attempts to enqueue a task. Returns false (and does NOT enqueue)
    // if the queue is already at max_queue_size_. The caller (main())
    // is responsible for deciding what to do on rejection; this class
    // has no knowledge of HTTP, sockets, or 503 responses.
    bool try_submit(std::function<void()> task)
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (task_queue_.size() >= max_queue_size_)
            return false;
        task_queue_.push(std::move(task));
        lock.unlock();
        cv_.notify_one();
        return true;
    }

    // Signals all worker threads to stop after finishing any currently
    // queued tasks, then joins them. Not currently called anywhere in
    // main() (the server runs until killed), but provided for
    // correctness and so the destructor has well-defined behavior if
    // the pool is ever destroyed while threads are running (e.g. in a
    // future test harness).
    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_)
            t.join();
    }

private:
    void worker_loop()
    {
        while (true)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this]
                         { return !task_queue_.empty() || shutdown_; });

                if (shutdown_ && task_queue_.empty())
                    return;

                task = std::move(task_queue_.front());
                task_queue_.pop();
            } // lock released here, before running the task

            task(); // executed outside the lock
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    size_t max_queue_size_;
    bool shutdown_;
};

// ---------------------------------------------------------------------
// handle_connection
//
// NEW as of Week 2 / Day 6: this is the entire per-connection block that
// used to live inline in main()'s accept loop, relocated verbatim into
// its own function so it can be handed to the thread pool as a task.
// The keep-alive logic, request parsing, routing, and static file
// serving inside this function are UNCHANGED from the Day 5 version;
// only the surrounding location moved. public_dir is taken by value
// (see handover doc Day 6 section for why: it's a small, immutable
// string, and by-value capture removes any cross-thread lifetime
// question entirely, at negligible copy cost).
void handle_connection(int client_fd, std::string public_dir)
{
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Per-connection state. Declared here, outside the keep-alive loop,
    // so it persists across multiple requests on the same connection.
    std::string leftover;

    while (true) // keep-alive loop: one iteration per request
    {
        auto [raw, status] = read_request(client_fd, leftover);

        if (status == ReadStatus::ClientClosed)
            break; // peer already closed, nothing to send, tear down

        if (status == ReadStatus::TimedOut)
        {
            // Send 408 before closing. Best-effort: the peer may have
            // already half-closed or gone away, in which case send()
            // fails silently (return value ignored) and we close
            // regardless. This is not an error in our logic, it's a
            // race inherent to TCP; nothing to retry or recover.
            const std::string body = "<h1>408 Request Timeout</h1>";
            std::string response =
                "HTTP/1.1 408 Request Timeout\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"
                                              "Connection: close\r\n"
                                              "\r\n" +
                body;
            send(client_fd, response.c_str(), response.size(), 0);
            break;
        }

        std::cout << raw << "\n";

        auto request_line = parse_request_line(raw);

        std::string body;
        int status_code;
        std::string status_text;
        std::string content_type = "text/html"; // default; overridden for static files

        if (!request_line)
        {
            body = "<h1>400 Bad Request</h1>";
            status_code = 400;
            status_text = "Bad Request";
        }
        else if (!has_header(raw, "host"))
        {
            body = "<h1>400 Bad Request</h1>";
            status_code = 400;
            status_text = "Bad Request";
        }
        else if (request_line->method != "GET" &&
                 request_line->method != "POST" &&
                 request_line->method != "HEAD")
        {
            body = "<h1>405 Method Not Allowed</h1>";
            status_code = 405;
            status_text = "Method Not Allowed";
        }
        else if (request_line->method == "POST" && !has_header(raw, "content-length"))
        {
            body = "<h1>411 Length Required</h1>";
            status_code = 411;
            status_text = "Length Required";
        }
        else
        {
            const std::string &path = request_line->path;
            std::cout << "Path: " << path << "\n";

            if (path == "/")
            {
                body = "<h1>Hello</h1>";
                status_code = 200;
                status_text = "OK";
            }
            else if (path == "/about")
            {
                body = "<h1>About Page</h1>";
                status_code = 200;
                status_text = "OK";
            }
            else
            {
                // Static file serving fallback. Reached only for paths
                // that are not one of the hardcoded routes above.
                //
                // Traversal guard runs on the raw request path, before
                // any disk path is constructed, and before the
                // filesystem is touched at all. This is deliberate:
                // correctness here must not depend on the OS refusing
                // to open a path that escapes public_dir; it depends
                // on never constructing or opening that path in the
                // first place. Rejects "/foo..bar.html" too (a literal
                // ".." substring in an otherwise normal filename) as a
                // false positive; that tradeoff is accepted as the
                // conservative side to err on.
                if (path.find("..") != std::string::npos)
                {
                    body = "<h1>400 Bad Request</h1>";
                    status_code = 400;
                    status_text = "Bad Request";
                }
                else
                {
                    // path already starts with '/' (enforced by
                    // parse_request_line), so this concatenation does
                    // not need an extra separator.
                    std::string disk_path = public_dir + path;

                    std::ifstream file(disk_path, std::ios::binary);
                    if (!file)
                    {
                        // Distinct from the hardcoded route-not-found
                        // 404 above: this means "no such file on disk
                        // under public_dir", not "no such route".
                        body = "<h1>404 File Not Found</h1>";
                        status_code = 404;
                        status_text = "Not Found";
                    }
                    else
                    {
                        body.assign((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
                        status_code = 200;
                        status_text = "OK";
                        content_type = get_mime_type(path);
                    }
                }
            }
        }

        bool is_head = request_line && request_line->method == "HEAD";

        // has_header only confirms the Connection header is present,
        // not its value, so the close-vs-keep-alive decision still
        // needs an explicit value check here rather than going
        // through has_header. This duplicates one lowercase pass over
        // the headers (has_header already did one to check for
        // presence of other headers), which is a known minor
        // redundancy, not a correctness issue: both passes operate on
        // the same immutable `raw` string and produce no side effects.
        bool client_requested_close = false;
        {
            std::string headers = raw.substr(0, raw.find("\r\n\r\n"));
            std::string lowered = headers;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c)
                           { return std::tolower(c); });
            client_requested_close = lowered.find("\r\nconnection: close") != std::string::npos;
        }

        // Close on: explicit client request, OR any response where
        // the server can no longer trust byte-boundary tracking on
        // this connection. 400 means the request line itself didn't
        // parse, so Content-Length math may never have run correctly.
        // 411 means a POST body's length is unknown, so any bytes
        // that follow in the buffer cannot be safely attributed to
        // this request or the next one. Continuing to trust `leftover`
        // after either case risks silently misparsing every subsequent
        // request on this connection.
        bool should_close = client_requested_close || status_code == 400 || status_code == 411;

        std::string response =
            "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
                                                                            "Content-Type: " +
            content_type + "\r\n"
                           "Content-Length: " +
            std::to_string(body.size()) + "\r\n"
                                          "Connection: " +
            (should_close ? "close" : "keep-alive") + "\r\n"
                                                      "\r\n";

        if (!is_head)
        {
            response += body;
        }

        send(client_fd, response.c_str(), response.size(), 0);

        if (should_close)
            break;
    }

    close(client_fd);
}

int main()
{
    // Resolved once at startup, not per-request/per-connection.
    // PROJECT_ROOT is injected by CMake at compile time
    // (target_compile_definitions, set to CMAKE_SOURCE_DIR), so this is
    // independent of the working directory the binary happens to be
    // launched from.
    const std::string public_dir = std::string(PROJECT_ROOT) + "/public";

    // NEW as of Week 2 / Day 6: thread pool constructed once before the
    // accept loop. hardware_concurrency() is a starting point tied to
    // the machine's actual core count, not a benchmarked-optimal value;
    // this server's work is I/O-bound (blocking recv/send), so more
    // threads than cores may well perform better once epoll removes the
    // blocking-read constraint. Treat this as a tunable, not a final
    // answer; the wrk benchmarking step is where this gets revisited.
    //
    // max_queue_size of 1000 is a placeholder starting point, not a
    // load-tested figure. It exists so a queue-full condition is
    // actually reachable and testable, and so memory growth under
    // sustained overload is bounded rather than open-ended. Revisit
    // alongside thread_count during benchmarking.
    unsigned int thread_count = std::thread::hardware_concurrency();
    ThreadPool pool(thread_count, /*max_queue_size=*/1000);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "bind() failed\n";
        return 1;
    }

    if (listen(server_fd, 128) < 0)
    {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "Listening on port 8080 with " << thread_count << " worker threads\n";

    while (true)
    {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            std::cerr << "accept() failed\n";
            continue;
        }

        // NEW as of Week 2 / Day 6: instead of running the connection
        // handler inline on the main thread (which blocked accept() from
        // servicing the next client until the current one finished), the
        // connection is now handed to the worker pool as a task, and the
        // main thread immediately loops back to accept() the next
        // client. public_dir is captured by value (cheap, immutable,
        // removes any cross-thread lifetime concern).
        bool submitted = pool.try_submit([client_fd, public_dir]
                                         { handle_connection(client_fd, public_dir); });

        if (!submitted)
        {
            // Queue is full. Per explicit decision: send a real 503
            // response before closing, rather than silently dropping
            // the connection with no explanation. This happens on the
            // main thread, not a worker, since the worker pool itself
            // is what's overloaded; the main thread is otherwise idle
            // at exactly this moment (it just finished accept()).
            const std::string body = "<h1>503 Service Unavailable</h1>";
            std::string response =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"
                                              "Connection: close\r\n"
                                              "\r\n" +
                body;
            send(client_fd, response.c_str(), response.size(), 0);
            close(client_fd);
        }
    }

    return 0;
}