cmake_minimum_required(VERSION 3.16)
project(http_server)
set(CMAKE_CXX_STANDARD 17)

add_executable(http_server
    src/main.cpp
    src/http_parsing.cpp
    src/router.cpp
    src/thread_pool.cpp
    src/connection.cpp
    src/server.cpp
)

target_include_directories(http_server PRIVATE include)
target_compile_definitions(http_server PRIVATE PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

find_package(Threads REQUIRED)
target_link_libraries(http_server PRIVATE Threads::Threads)