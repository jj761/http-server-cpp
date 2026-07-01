#include "thread_pool.hpp"

ThreadPool::ThreadPool(size_t thread_count, size_t max_queue_size)
    : max_queue_size_(max_queue_size), shutdown_(false)
{
    if (thread_count == 0)
        thread_count = 1;

    for (size_t i = 0; i < thread_count; ++i)
        workers_.emplace_back([this]
                              { worker_loop(); });
}

bool ThreadPool::try_submit(std::function<void()> task)
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (task_queue_.size() >= max_queue_size_)
        return false;
    task_queue_.push(std::move(task));
    lock.unlock();
    cv_.notify_one();
    return true;
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_)
        t.join();
}

void ThreadPool::worker_loop()
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
        }

        task();
    }
}