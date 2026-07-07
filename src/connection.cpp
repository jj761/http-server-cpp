#include "connection.hpp"
#include "http_parsing.hpp"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

std::unordered_map<int, std::shared_ptr<ConnectionState>> connections;
std::mutex connections_map_mutex;

// Shared log mutex, guards all std::cerr diagnostic writes across the
// codebase. Distinct from connection_mutex and connections_map_mutex
// so logging never contends with connection-state or map locking.
std::mutex log_mutex;

// Builds and writes one log line atomically. Callers must pass a
// fully-assembled string (e.g. via std::ostringstream) rather than
// chaining multiple operator<< calls directly on std::cerr, since
// separate calls can interleave across threads.
void log_line(const std::string &line)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << line;
}

ConnectionState::ConnectionState(int fd_, std::string public_dir_)
    : fd(fd_), public_dir(std::move(public_dir_)),
      last_active(time(nullptr)), closing(false) {}

void close_connection(int epoll_fd, const std::shared_ptr<ConnectionState> &conn)
{
    {
        std::lock_guard<std::mutex> lock(conn->connection_mutex);
        if (conn->closing)
        {
            return;
        }
        conn->closing = true;
    }

    int fd = conn->fd;
    {
        std::lock_guard<std::mutex> map_lock(connections_map_mutex);
        auto it = connections.find(fd);
        if (it != connections.end() && it->second.get() == conn.get())
            connections.erase(it);
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}
ReadOutcome read_available(int fd, std::string &leftover,
                           std::vector<std::string> &out_messages)
{
    char chunk[4096];
    while (true)
    {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0)
            return ReadOutcome::ClientClosed;
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return ReadOutcome::Drained;
            return ReadOutcome::Error;
        }
        leftover.append(chunk, n);
        while (auto msg = try_extract_message(leftover))
            out_messages.push_back(std::move(*msg));
    }
}
bool drain_output_locked(int epoll_fd, std::shared_ptr<ConnectionState> &conn)
{
    while (!conn->out_buffer.empty())
    {
        ssize_t n = send(conn->fd, conn->out_buffer.data(), conn->out_buffer.size(), 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                {
                    std::ostringstream oss;
                    oss << "[epollout-register] fd=" << conn->fd
                        << " remaining=" << conn->out_buffer.size() << "\n";
                    log_line(oss.str());
                }
                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = conn->fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                return true;
            }
            {
                std::ostringstream oss;
                oss << "[write-error] fd=" << conn->fd
                    << " errno=" << strerror(errno) << "\n";
                log_line(oss.str());
            }
            return false;
        }
        conn->out_buffer.erase(0, n);
        conn->last_active = time(nullptr);
    }
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = conn->fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
    return true;
}
bool try_write(int epoll_fd, std::shared_ptr<ConnectionState> conn, const std::string &data)
{
    std::lock_guard<std::mutex> lock(conn->connection_mutex);
    if (conn->closing)
        return true;
    conn->out_buffer += data;
    return drain_output_locked(epoll_fd, conn);
}