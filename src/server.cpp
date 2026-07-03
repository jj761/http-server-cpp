#include "server.hpp"
#include "connection.hpp"
#include "thread_pool.hpp"
#include "router.hpp"
#include <iostream>
#include <sstream>
#include <mutex>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <thread>
#include <chrono>

constexpr int QUEUE_TIMEOUT_SECONDS = 30;
constexpr int IDLE_TIMEOUT_SECONDS = 5;

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
                int threshold = conn->dequeued ? IDLE_TIMEOUT_SECONDS : QUEUE_TIMEOUT_SECONDS;
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
            close_connection(epoll_fd, fd);
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
                    int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0)
                        break;
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
                close_connection(epoll_fd, fd);
                continue;
            }

            if (events[i].events & EPOLLOUT)
            {
                bool drain_failed = false;
                {
                    std::lock_guard<std::mutex> lock(conn->connection_mutex);
                    if (!conn->closing)
                        drain_failed = !drain_output_locked(epoll_fd, conn);
                }
                if (drain_failed)
                {
                    {
                        std::ostringstream oss;
                        oss << "[epollout-drain-failed] fd=" << fd << "\n";
                        log_line(oss.str());
                    }
                    close_connection(epoll_fd, fd);
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
                    conn->dequeued = false;
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
                    close_connection(epoll_fd, fd);
                    continue;
                }

                for (auto &msg : messages)
                {
                    auto enqueue_time = std::chrono::steady_clock::now();
                    bool submitted = pool.try_submit([epoll_fd, conn, msg = std::move(msg), enqueue_time]() mutable
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
                            conn->dequeued = true;
                        }
                        ProcessResult result = process_request(msg, conn->public_dir);
                        bool write_ok = try_write(epoll_fd, conn, result.response);
                        if (!write_ok || result.should_close)
                            close_connection(epoll_fd, conn->fd); });
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
                        close_connection(epoll_fd, fd);
                    }
                }
            }
        }
    }

    shutdown_flag = true;
    sweep_thread.join();
}