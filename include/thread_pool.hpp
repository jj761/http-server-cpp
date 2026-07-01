#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class ThreadPool
{
public:
    explicit ThreadPool(size_t thread_count, size_t max_queue_size);
    bool try_submit(std::function<void()> task);
    ~ThreadPool();

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    size_t max_queue_size_;
    bool shutdown_;
};