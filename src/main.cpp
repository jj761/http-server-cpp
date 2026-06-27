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

struct RequestLine
{
    std::string method;
    std::string path;
};

// Consolidates what were previously two separate functions (parse_path,
// parse_method), each independently calling request.find(' ') on the
// same string. This was flagged in the Day 2/3 handover as Open Issue #2,
// deliberately deferred until the full set of needed request-line fields
// (method, path) was known. It now is, so this is the single
// consolidation point: one scan of the request line, both fields
// extracted together. Returns nullopt if the request line is malformed
// in any way (missing spaces, path not starting with '/').
std::optional<RequestLine> parse_request_line(const std::string &request)
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

    RequestLine result;
    result.method = request.substr(0, first_space);
    result.path = path;
    return result;
}

// Checks for the presence of a header by name, case-insensitively,
// anchored to "\r\n" + name so it cannot false-match a longer header
// name that merely contains this one as a substring (e.g. X-Custom-Host
// must not match a search for "host"). This consolidates what were
// previously three independent lowercase-and-search implementations
// (has_host_header, has_content_length_header, and an inline check for
// "Connection: close" in main()), flagged in the Day 3 handover as Open
// Issue #3 once a third instance appeared. lowercase_name must already
// be lowercase and must NOT include the trailing colon; it is added here.
bool has_header(const std::string &request, const std::string &lowercase_name)
{
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = (header_end == std::string::npos)
                              ? request
                              : request.substr(0, header_end);

    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    return headers.find("\r\n" + lowercase_name + ":") != std::string::npos;
}

// Attempts to slice exactly one complete HTTP message out of `buffer`.
// Returns the message if one is fully present, std::nullopt otherwise.
// On success, `buffer` is mutated to contain only the bytes AFTER the
// sliced message (i.e. it becomes the leftover for the next call).
//
// This is the core of the keep-alive fix: it operates purely on data
// already in memory, performs no recv() calls, and is what makes the
// "next request's bytes already sitting in leftover" case work without
// touching the socket again.
std::optional<std::string> try_extract_message(std::string &buffer)
{
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos)
        return std::nullopt; // headers not fully received yet

    size_t content_length = 0;
    std::string cl_header = "Content-Length: ";
    size_t cl_pos = buffer.find(cl_header);
    if (cl_pos != std::string::npos && cl_pos < header_end)
    {
        size_t cl_end = buffer.find("\r\n", cl_pos);
        content_length = std::stoul(buffer.substr(cl_pos + cl_header.size(), cl_end - cl_pos - cl_header.size()));
    }

    size_t message_end = header_end + 4 + content_length; // 4 = strlen("\r\n\r\n")
    size_t body_received = buffer.size() - (header_end + 4);

    if (body_received < content_length)
        return std::nullopt; // body still incomplete

    std::string message = buffer.substr(0, message_end);
    buffer.erase(0, message_end); // leftover stays in buffer for the next call
    return message;
}

enum class ReadStatus
{
    Ok,           // a complete message was returned
    ClientClosed, // recv() returned 0, peer closed the connection
    TimedOut      // recv() returned EAGAIN/EWOULDBLOCK
};

// Reads exactly one complete HTTP message from client_fd.
// `leftover` is caller-owned, persists across calls for the lifetime of
// the connection, and holds any bytes read past the end of the current
// message (e.g. the start of a pipelined next request).
//
// Contract:
//   - On ReadStatus::Ok, the return string is a complete message and
//     `leftover` holds whatever (possibly empty) bytes came after it.
//   - On ClientClosed or TimedOut, the return string is empty and the
//     caller must close the connection. `leftover` is left as-is; it is
//     irrelevant once the connection is closing.
std::pair<std::string, ReadStatus> read_request(int client_fd, std::string &leftover)
{
    // Case 1: a full message is already sitting in leftover from the
    // previous call (pipelined request). No recv() needed at all.
    if (auto msg = try_extract_message(leftover))
        return {*msg, ReadStatus::Ok};

    char chunk[4096];

    while (true)
    {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);

        if (n == 0)
            return {"", ReadStatus::ClientClosed};

        if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                std::cerr << "read_request: timed out waiting for data\n";
            return {"", ReadStatus::TimedOut};
        }

        leftover.append(chunk, n);

        if (auto msg = try_extract_message(leftover))
            return {*msg, ReadStatus::Ok};

        // else: headers or body still incomplete, loop and recv() more
    }
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

        // Per-connection state. Declared here, outside the keep-alive loop,
        // so it persists across multiple requests on the same connection.
        std::string leftover;

        while (true) // keep-alive loop: one iteration per request
        {
            auto [raw, status] = read_request(client_fd, leftover);

            if (status == ReadStatus::ClientClosed)
                break; // peer already closed, nothing to send, tear down

            if (status == ReadStatus::TimedOut)
            {
                // Send 408 before closing. Best-effort: the peer may have
                // already half-closed or gone away, in which case send()
                // fails silently (return value ignored) and we close
                // regardless. This is not an error in our logic, it's a
                // race inherent to TCP; nothing to retry or recover.
                const std::string body = "<h1>408 Request Timeout</h1>";
                std::string response =
                    "HTTP/1.1 408 Request Timeout\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " +
                    std::to_string(body.size()) + "\r\n"
                                                  "Connection: close\r\n"
                                                  "\r\n" +
                    body;
                send(client_fd, response.c_str(), response.size(), 0);
                break;
            }

            std::cout << raw << "\n";

            auto request_line = parse_request_line(raw);

            std::string body;
            int status_code;
            std::string status_text;

            if (!request_line)
            {
                body = "<h1>400 Bad Request</h1>";
                status_code = 400;
                status_text = "Bad Request";
            }
            else if (!has_header(raw, "host"))
            {
                body = "<h1>400 Bad Request</h1>";
                status_code = 400;
                status_text = "Bad Request";
            }
            else if (request_line->method != "GET" &&
                     request_line->method != "POST" &&
                     request_line->method != "HEAD")
            {
                body = "<h1>405 Method Not Allowed</h1>";
                status_code = 405;
                status_text = "Method Not Allowed";
            }
            else if (request_line->method == "POST" && !has_header(raw, "content-length"))
            {
                body = "<h1>411 Length Required</h1>";
                status_code = 411;
                status_text = "Length Required";
            }
            else
            {
                const std::string &path = request_line->path;
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

            bool is_head = request_line && request_line->method == "HEAD";

            // has_header only confirms the Connection header is present,
            // not its value, so the close-vs-keep-alive decision still
            // needs an explicit value check here rather than going
            // through has_header. This duplicates one lowercase pass over
            // the headers (has_header already did one to check for
            // presence of other headers), which is a known minor
            // redundancy, not a correctness issue: both passes operate on
            // the same immutable `raw` string and produce no side effects.
            bool client_requested_close = false;
            {
                std::string headers = raw.substr(0, raw.find("\r\n\r\n"));
                std::string lowered = headers;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c)
                               { return std::tolower(c); });
                client_requested_close = lowered.find("\r\nconnection: close") != std::string::npos;
            }

            // Close on: explicit client request, OR any response where
            // the server can no longer trust byte-boundary tracking on
            // this connection. 400 means the request line itself didn't
            // parse, so Content-Length math may never have run correctly.
            // 411 means a POST body's length is unknown, so any bytes
            // that follow in the buffer cannot be safely attributed to
            // this request or the next one (Open Issue #1). Continuing
            // to trust `leftover` after either case risks silently
            // misparsing every subsequent request on this connection.
            bool should_close = client_requested_close || status_code == 400 || status_code == 411;

            std::string response =
                "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
                                                                                "Content-Type: text/html\r\n"
                                                                                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"
                                              "Connection: " +
                (should_close ? "close" : "keep-alive") + "\r\n"
                                                          "\r\n";

            if (!is_head)
            {
                response += body;
            }

            send(client_fd, response.c_str(), response.size(), 0);

            if (should_close)
                break;
        }

        close(client_fd);
    }

    return 0;
}