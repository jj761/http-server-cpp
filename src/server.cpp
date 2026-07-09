#include "server.hpp"
#include "connection.hpp"
#include "thread_pool.hpp"
#include "router.hpp"
#include <iostream>
#include <sstream>
#include <mutex>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <thread>
#include <chrono>

constexpr int QUEUE_TIMEOUT_SECONDS = 30;
constexpr int IDLE_TIMEOUT_SECONDS = 5;
// [Guessing] — not derived from a measured slow-client model, only checked
// against the Day 15 Step 4 repro (200KB/s throttle, 10MB file, observed
// register->fired gaps of ~14.7s and ~7.9s). Revisit if Issue E or real
// traffic shows this is wrong in either direction.
constexpr int DRAINING_TIMEOUT_SECONDS = 30;

// log_mutex and log_line() are declared in connection.hpp and defined
// once in connection.cpp, shared across all translation units so every
// std::cerr diagnostic write in the codebase goes through the same lock.

void timeout_sweep(int epoll_fd, std::atomic<bool> &shutdown_flag)
{
    while (!shutdown_flag)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::vector<std::shared_ptr<ConnectionState>> snapshot;
        {
            std::lock_guard<std::mutex> map_lock(connections_map_mutex);
            snapshot.reserve(connections.size());
            for (auto &entry : connections)
                snapshot.push_back(entry.second);
        }
        time_t now = time(nullptr);
        for (auto &conn : snapshot)
        {
            bool timed_out = false;
            int fd = -1;
            {
                std::lock_guard<std::mutex> lock(conn->connection_mutex);
                if (conn->closing)
                    continue;
                if (conn->state == ConnState::InFlight)
                    continue; // actively being processed, never subject to timeout
                int threshold;
                if (conn->state == ConnState::Queued)
                    threshold = QUEUE_TIMEOUT_SECONDS;
                else if (conn->state == ConnState::Draining)
                    threshold = DRAINING_TIMEOUT_SECONDS;
                else
                    threshold = IDLE_TIMEOUT_SECONDS;
                if (now - conn->last_active >= threshold)
                {
                    timed_out = true;
                    fd = conn->fd;
                }
            }
            if (!timed_out)
                continue;
            {
                std::ostringstream oss;
                oss << "[idle-timeout] fd=" << fd << "\n";
                log_line(oss.str());
            }
            const std::string body = "<h1>408 Request Timeout</h1>";
            std::string response =
                "HTTP/1.1 408 Request Timeout\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"
                                              "Connection: close\r\n"
                                              "\r\n" +
                body;
            send(fd, response.c_str(), response.size(), 0);
            close_connection(epoll_fd, conn);
        }
    }
}

void run_epoll_loop(int listen_fd, ThreadPool &pool, const std::string &public_dir)
{
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        std::cerr << "epoll_create1() failed\n";
        return;
    }

    epoll_event listen_ev{};
    listen_ev.events = EPOLLIN | EPOLLET;
    listen_ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_ev);

    std::atomic<bool> shutdown_flag(false);
    std::thread sweep_thread(timeout_sweep, epoll_fd, std::ref(shutdown_flag));

    // Soft connection ceiling, derived from the process's actual fd limit
    // (RLIMIT_NOFILE) rather than a hardcoded constant, so this adapts
    // correctly whether the process is run under a constrained ulimit (e.g.
    // a test harness) or a production ceiling (e.g. ulimit -n 65536).
    // Headroom (100) is reserved for fds this process needs outside of
    // client connections: the listening socket, epoll_fd, the log file,
    // stdin/stdout/stderr, and any static files open for serving at a given
    // moment. [Guessing] on the exact headroom value -- not measured against
    // this process's actual non-connection fd usage, just a conservative
    // round number. Revisit if this proves too tight or too loose in
    // practice.
    rlimit rl{};
    size_t max_connections_soft = 1024; // fallback if getrlimit fails
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 100)
        max_connections_soft = static_cast<size_t>(rl.rlim_cur) - 100;
    {
        std::ostringstream oss;
        oss << "[startup] rlimit_nofile=" << rl.rlim_cur
            << " max_connections_soft=" << max_connections_soft << "\n";
        log_line(oss.str());
    }

    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            std::cerr << "epoll_wait() failed\n";
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;

            if (fd == listen_fd)
            {
                while (true)
                {
                    // Soft ceiling check: stop accepting new connections this
                    // cycle if we're within the reserved headroom of the fd
                    // limit. Existing connections continue being served
                    // normally -- this only affects new accept() calls.
                    // Edge-triggered epoll will re-notify EPOLLIN on the
                    // listen_fd on the next event loop iteration as long as
                    // the kernel's accept queue remains non-empty, so
                    // pending connections are not silently dropped, just
                    // deferred until fd headroom frees up.
                    {
                        std::lock_guard<std::mutex> lock(connections_map_mutex);
                        if (connections.size() >= max_connections_soft)
                        {
                            std::ostringstream oss;
                            oss << "[accept-throttled] connections=" << connections.size()
                                << " soft_limit=" << max_connections_soft << "\n";
                            log_line(oss.str());
                            break;
                        }
                    }
                    int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            std::ostringstream oss;
                            oss << "[accept-error] errno=" << strerror(errno) << "\n";
                            log_line(oss.str());
                        }
                        break;
                    }
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                    auto conn = std::make_shared<ConnectionState>(client_fd, public_dir);
                    {
                        std::lock_guard<std::mutex> lock(connections_map_mutex);
                        connections[client_fd] = conn;
                    }
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                }
                continue;
            }

            std::shared_ptr<ConnectionState> conn;
            {
                std::lock_guard<std::mutex> lock(connections_map_mutex);
                auto it = connections.find(fd);
                if (it == connections.end())
                    continue;
                conn = it->second;
            }

            if (events[i].events & (EPOLLHUP | EPOLLERR))
            {
                {
                    std::ostringstream oss;
                    oss << "[epollhup-err] fd=" << fd
                        << " events=" << events[i].events << "\n";
                    log_line(oss.str());
                }
                close_connection(epoll_fd, conn);
                continue;
            }

            if (events[i].events & EPOLLOUT)
            {
                DrainResult drain_result = DrainResult::Complete; // no-op if closing
                {
                    std::lock_guard<std::mutex> lock(conn->connection_mutex);
                    if (!conn->closing)
                    {
                        drain_result = drain_output_locked(epoll_fd, conn);
                        if (drain_result == DrainResult::Complete)
                            conn->state = ConnState::Idle;
                        // Pending: leave as Draining, no assignment needed
                    }
                }
                if (drain_result == DrainResult::Failed)
                {
                    {
                        std::ostringstream oss;
                        oss << "[epollout-drain-failed] fd=" << fd << "\n";
                        log_line(oss.str());
                    }
                    close_connection(epoll_fd, conn);
                    continue;
                }
                {
                    std::ostringstream oss;
                    oss << "[epollout-fired] fd=" << fd << "\n";
                    log_line(oss.str());
                }
            }

            if (events[i].events & EPOLLIN)
            {
                std::vector<std::string> messages;
                ReadOutcome outcome;
                {
                    std::lock_guard<std::mutex> lock(conn->connection_mutex);
                    if (conn->closing)
                        continue;
                    conn->last_active = time(nullptr);
                    conn->state = ConnState::Queued;
                    outcome = read_available(fd, conn->leftover, messages);
                }

                if (outcome == ReadOutcome::ClientClosed || outcome == ReadOutcome::Error)
                {
                    {
                        std::ostringstream oss;
                        oss << "[read-outcome] fd=" << fd
                            << " outcome=" << (outcome == ReadOutcome::ClientClosed ? "ClientClosed" : "Error")
                            << "\n";
                        log_line(oss.str());
                    }
                    close_connection(epoll_fd, conn);
                    continue;
                }

                // Issue "pipelined response ordering": all messages extracted from this
                // single read event are submitted as ONE task and processed sequentially
                // inside it, not one task per message. This guarantees in-order responses
                // for messages that arrive together in one read (the demonstrated bug
                // case), and, combined with the per-connection sequence gate below,
                // also across messages split over separate epoll read events on the
                // same connection.
                if (!messages.empty())
                {
                    uint64_t batch_seq;
                    {
                        std::lock_guard<std::mutex> lock(conn->connection_mutex);
                        batch_seq = conn->next_extract_seq++;
                    }
                    auto enqueue_time = std::chrono::steady_clock::now();
                    bool submitted = pool.try_submit([epoll_fd, conn, messages = std::move(messages), enqueue_time, batch_seq]() mutable
                                                     {
        // Enforce strict per-connection FIFO ordering: don't process or
        // write anything for this batch until every earlier-extracted
        // batch on this same connection has finished. Different
        // connections are entirely unaffected by this wait.
        {
            std::unique_lock<std::mutex> lock(conn->connection_mutex);
            conn->write_turn_cv.wait(lock, [&] {
                return conn->closing || conn->next_write_seq == batch_seq;
            });
            if (conn->closing)
            {
                lock.unlock();
                conn->write_turn_cv.notify_all();
                return;
            }
        }

        for (auto &msg : messages)
        {
            auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - enqueue_time).count();
            {
                std::ostringstream oss;
                oss << "[dequeue] fd=" << conn->fd << " wait_ms=" << wait_ms << "\n";
                log_line(oss.str());
            }
            {
                std::lock_guard<std::mutex> lock(conn->connection_mutex);
                conn->last_active = time(nullptr);
                conn->state = ConnState::InFlight;
            }

            // TEST-ONLY — Issue A deterministic delay test. Remove after verification.
            {
                auto marker = msg.find("/__test_delay");
                if (marker != std::string::npos)
                {
                    int seconds = 10;
                    auto line_end = msg.find(" HTTP/", marker);
                    auto pos = msg.find("seconds=", marker);
                    if (pos != std::string::npos && (line_end == std::string::npos || pos < line_end))
                        seconds = std::atoi(msg.c_str() + pos + 8);
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                }
            }

            ProcessResult result = process_request(msg, conn->public_dir);
            DrainResult drain_result = try_write(epoll_fd, conn, result.response);
            {
                std::lock_guard<std::mutex> lock(conn->connection_mutex);
                if (!conn->closing)
                {
                    conn->state = (drain_result == DrainResult::Pending)
                                       ? ConnState::Draining
                                       : ConnState::Idle;
                    conn->last_active = time(nullptr);
                }
            }
            if (drain_result == DrainResult::Failed || result.should_close)
            {
                close_connection(epoll_fd, conn);
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(conn->connection_mutex);
            conn->next_write_seq = batch_seq + 1;
        }
        conn->write_turn_cv.notify_all(); });
                    if (!submitted)
                    {
                        {
                            std::ostringstream oss;
                            oss << "[queue-full] rejecting fd=" << fd << "\n";
                            log_line(oss.str());
                        }
                        const std::string body = "<h1>503 Service Unavailable</h1>";
                        std::string response =
                            "HTTP/1.1 503 Service Unavailable\r\n"
                            "Content-Type: text/html\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) + "\r\n"
                                                          "Connection: close\r\n"
                                                          "\r\n" +
                            body;
                        try_write(epoll_fd, conn, response);
                        close_connection(epoll_fd, conn);
                    }
                }
            }
        }
    }

    shutdown_flag = true;
    sweep_thread.join();
}