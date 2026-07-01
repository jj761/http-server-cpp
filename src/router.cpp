#include "router.hpp"
#include "http_parsing.hpp"

#include <fstream>
#include <algorithm>
#include <cctype>

ProcessResult process_request(const std::string &raw, const std::string &public_dir)
{
    auto request_line = parse_request_line(raw);

    std::string body;
    int status_code;
    std::string status_text;
    std::string content_type = "text/html";

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
            if (path.find("..") != std::string::npos)
            {
                body = "<h1>400 Bad Request</h1>";
                status_code = 400;
                status_text = "Bad Request";
            }
            else
            {
                std::string disk_path = public_dir + path;
                std::ifstream file(disk_path, std::ios::binary);
                if (!file)
                {
                    body = "<h1>404 File Not Found</h1>";
                    status_code = 404;
                    status_text = "Not Found";
                }
                else
                {
                    body.assign((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
                    status_code = 200;
                    status_text = "OK";
                    content_type = get_mime_type(path);
                }
            }
        }
    }

    bool is_head = request_line && request_line->method == "HEAD";

    bool client_requested_close = false;
    {
        std::string headers = raw.substr(0, raw.find("\r\n\r\n"));
        std::string lowered = headers;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        client_requested_close = lowered.find("\r\nconnection: close") != std::string::npos;
    }

    bool should_close = client_requested_close || status_code == 400 || status_code == 411;

    std::string response =
        "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n"
                                                                        "Content-Type: " +
        content_type + "\r\n"
                       "Content-Length: " +
        std::to_string(body.size()) + "\r\n"
                                      "Connection: " +
        (should_close ? "close" : "keep-alive") + "\r\n"
                                                  "\r\n";

    if (!is_head)
        response += body;

    return {response, should_close};
}