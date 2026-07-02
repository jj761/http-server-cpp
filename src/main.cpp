#include "thread_pool.hpp"
#include "server.hpp"
#include <iostream>
#include <thread>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

int main()
{
    const std::string public_dir = std::string(PROJECT_ROOT) + "/public";

    unsigned int thread_count = std::thread::hardware_concurrency();
    ThreadPool pool(thread_count, /*max_queue_size=*/1000);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "bind() failed: " << strerror(errno) << "\n";
        return 1;
    }

    if (listen(server_fd, 128) < 0)
    {
        std::cerr << "listen() failed: " << strerror(errno) << "\n";
        return 1;
    }

    std::cout << "Listening on port 8080 with " << thread_count << " worker threads (epoll, edge-triggered)\n";

    run_epoll_loop(server_fd, pool, public_dir);

    return 0;
}