#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <ctime>

enum class ConnState
{
    Idle,
    Queued,
    InFlight
};

struct ConnectionState
{
    int fd;
    std::string public_dir;
    std::string leftover;
    std::string out_buffer;
    time_t last_active;
    bool closing;
    std::mutex connection_mutex;
    ConnState state = ConnState::Idle; // replaces: bool dequeued = false;
    ConnectionState(int fd_, std::string public_dir_);
};
// Global connection table. Declared here, defined once in connection.cpp
// (see note below on why this must NOT be defined in the header).
extern std::unordered_map<int, std::shared_ptr<ConnectionState>> connections;
extern std::mutex connections_map_mutex;
// Single teardown funnel for a connection, callable from any thread,
// for any reason (read error, client close, write error, timeout).
//
// Takes the caller's own shared_ptr<ConnectionState>, not a raw fd. This
// is required for correctness: the closing check runs on the object the
// caller actually holds, under its own connection_mutex, BEFORE any map
// lookup by fd number. Passing a bare int fd here would reopen a fd-reuse
// race (Issue G) where a stale caller's fd number could have already been
// reassigned by the OS to an unrelated, live connection -- an fd-keyed
// lookup can't tell the difference, but checking closing on the caller's
// own object first can.
void close_connection(int epoll_fd, const std::shared_ptr<ConnectionState> &conn);
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

extern std::mutex log_mutex;
void log_line(const std::string &line);