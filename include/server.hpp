#pragma once

#include <string>
#include <atomic>

class ThreadPool;

void timeout_sweep(int epoll_fd, std::atomic<bool> &shutdown_flag);
void run_epoll_loop(int listen_fd, ThreadPool &pool, const std::string &public_dir);