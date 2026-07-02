#include "connection.hpp"
#include "http_parsing.hpp"
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

std::unordered_map<int, std::shared_ptr<ConnectionState>> connections;
std::mutex connections_map_mutex;

ConnectionState::ConnectionState(int fd_, std::string public_dir_)
    : fd(fd_), public_dir(std::move(public_dir_)),
      last_active(time(nullptr)), closing(false) {}

void close_connection(int epoll_fd, int fd)
{
    std::shared_ptr<ConnectionState> conn;
    {
        std::lock_guard<std::mutex> map_lock(connections_map_mutex);
        auto it = connections.find(fd);
        if (it == connections.end())
            return;
        conn = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(conn->connection_mutex);
        if (conn->closing)
            return;
        conn->closing = true;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    std::lock_guard<std::mutex> map_lock(connections_map_mutex);
    connections.erase(fd);
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
                std::cerr << "[epollout-register] fd=" << conn->fd
                          << " remaining=" << conn->out_buffer.size() << "\n";
                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = conn->fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                return true;
            }
            std::cerr << "[write-error] fd=" << conn->fd
                      << " errno=" << strerror(errno) << "\n";
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