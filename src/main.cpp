#include <iostream>
#include <string>
#include <cstring>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

std::optional<std::string> parse_path(const std::string &request)
{
    size_t first_space = request.find(' ');
    if (first_space == std::string::npos)
        return std::nullopt;

    size_t second_space = request.find(' ', first_space + 1);
    if (second_space == std::string::npos)
        return std::nullopt;

    std::string path = request.substr(first_space + 1, second_space - first_space - 1);
    if (path.empty() || path[0] != '/')
        return std::nullopt;

    return path;
}

std::optional<std::string> parse_method(const std::string &request)
{
    size_t first_space = request.find(' ');
    if (first_space == std::string::npos)
        return std::nullopt;

    return request.substr(0, first_space);
}

bool has_host_header(const std::string &request)
{
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = (header_end == std::string::npos)
                              ? request
                              : request.substr(0, header_end);

    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    return headers.find("\r\nhost:") != std::string::npos;
}

bool has_content_length_header(const std::string &request)
{
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = (header_end == std::string::npos)
                              ? request
                              : request.substr(0, header_end);

    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    return headers.find("\r\ncontent-length:") != std::string::npos;
}

std::string read_request(int client_fd)
{
    std::string buffer;
    char chunk[4096];

    while (true)
    {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);

        if (n == 0)
            break;

        if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                std::cerr << "read_request: timed out waiting for headers\n";
            break;
        }

        buffer.append(chunk, n);

        size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos)
            continue;

        size_t content_length = 0;
        std::string cl_header = "Content-Length: ";
        size_t cl_pos = buffer.find(cl_header);
        if (cl_pos != std::string::npos)
        {
            size_t cl_end = buffer.find("\r\n", cl_pos);
            content_length = std::stoul(buffer.substr(cl_pos + cl_header.size(), cl_end - cl_pos - cl_header.size()));
        }

        size_t body_received = buffer.size() - (header_end + 4);
        if (body_received >= content_length)
            break;
    }

    return buffer;
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        std::cerr << "socket() failed\n";
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "bind() failed\n";
        return 1;
    }

    if (listen(server_fd, 128) < 0)
    {
        std::cerr << "listen() failed\n";
        return 1;
    }

    std::cout << "Listening on port 8080\n";

    while (true)
    {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            std::cerr << "accept() failed\n";
            continue;
        }

        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        std::string raw = read_request(client_fd);
        std::cout << raw << "\n";

        auto parsed_path = parse_path(raw);
        auto parsed_method = parse_method(raw);

        std::string body;
        int status_code;
        std::string status_text;

        if (!parsed_path)
        {
            body = "<h1>400 Bad Request</h1>";
            status_code = 400;
            status_text = "Bad Request";
        }
        else if (!has_host_header(raw))
        {
            body = "<h1>400 Bad Request</h1>";
            status_code = 400;
            status_text = "Bad Request";
        }
        else if (parsed_method &&
                 *parsed_method != "GET" &&
                 *parsed_method != "POST" &&
                 *parsed_method != "HEAD")
        {
            body = "<h1>405 Method Not Allowed</h1>";
            status_code = 405;
            status_text = "Method Not Allowed";
        }
        else if (parsed_method && *parsed_method == "POST" && !has_content_length_header(raw))
        {
            body = "<h1>411 Length Required</h1>";
            status_code = 411;
            status_text = "Length Required";
        }
        else
        {
            const std::string &path = *parsed_path;
            std::cout << "Path: " << path << "\n";

            if (path == "/")
            {
                body = "<h1>Hello</h1>";
                status_code = 200;
                status_text = "OK";
            }
            else if (path == "/about")
            {
                body = "<h1>About Page</h1>";
                status_code = 200;
                status_text = "OK";
            }
            else
            {
                body = "<h1>404 Not Found</h1>";
                status_code = 404;
                status_text = "Not Found";
            }
        }

        bool is_head = parsed_method && *parsed_method == "HEAD";

        std::string response =
            "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
                                                                            "Content-Type: text/html\r\n"
                                                                            "Content-Length: " +
            std::to_string(body.size()) + "\r\n"
                                          "Connection: close\r\n"
                                          "\r\n";

        if (!is_head)
        {
            response += body;
        }

        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
    }

    return 0;
}