#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <cstdint>
#include <ctime>

enum class ConnState
{
    Idle,
    Queued,
    InFlight,
    Draining
};

enum class DrainResult
{
    Complete,
    Pending,
    Failed
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

    // Per-connection pipelined-batch ordering. A "batch" is the set of
    // messages extracted from a single read event. next_extract_seq is
    // assigned once per batch at extraction time; next_write_seq gates
    // batches so they process/write strictly in the order they were
    // extracted, even if their underlying tasks run on different worker
    // threads and would otherwise race. write_turn_cv wakes a waiting
    // batch either when the batch ahead of it finishes, or when the
    // connection is closed from any code path (see close_connection).
    uint64_t next_extract_seq = 0;
    uint64_t next_write_seq = 0;
    std::condition_variable write_turn_cv;

    ConnectionState(int fd_, std::string public_dir_);
};

extern std::unordered_map<int, std::shared_ptr<ConnectionState>> connections;
extern std::mutex connections_map_mutex;

void close_connection(int epoll_fd, const std::shared_ptr<ConnectionState> &conn);

enum class ReadOutcome
{
    ClientClosed,
    Error,
    Drained
};

ReadOutcome read_available(int fd, std::string &leftover,
                           std::vector<std::string> &out_messages);

DrainResult drain_output_locked(int epoll_fd, std::shared_ptr<ConnectionState> &conn);

DrainResult try_write(int epoll_fd, std::shared_ptr<ConnectionState> conn, const std::string &data);

extern std::mutex log_mutex;
void log_line(const std::string &line);