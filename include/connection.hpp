#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <ctime>

struct ConnectionState
{
    int fd;
    std::string public_dir;
    std::string leftover;
    std::string out_buffer;
    time_t last_active;
    bool closing;
    std::mutex connection_mutex;

    ConnectionState(int fd_, std::string public_dir_);
};

// Global connection table. Declared here, defined once in connection.cpp
// (see note below on why this must NOT be defined in the header).
extern std::unordered_map<int, std::shared_ptr<ConnectionState>> connections;
extern std::mutex connections_map_mutex;

// Single teardown funnel for a connection, callable from any thread,
// for any reason (read error, client close, write error, timeout).
// Uses ConnectionState::closing under connection_mutex to guarantee
// only one caller executes the epoll_ctl/close/erase sequence per fd.
void close_connection(int epoll_fd, int fd);

enum class ReadOutcome
{
    ClientClosed,
    Error,
    Drained
};

// Edge-triggered read drain. Loops internally until EAGAIN. May
// deliver zero, one, or multiple complete messages per call.
ReadOutcome read_available(int fd, std::string &leftover,
                           std::vector<std::string> &out_messages);

// Caller must already hold conn->connection_mutex.
bool drain_output_locked(int epoll_fd, std::shared_ptr<ConnectionState> &conn);

// Appends to out_buffer and attempts a drain. Returns false only on a
// hard send() error, not on EAGAIN (deferred write, handled via
// EPOLLOUT arming inside drain_output_locked).
bool try_write(int epoll_fd, std::shared_ptr<ConnectionState> conn, const std::string &data);