#include "http_parsing.hpp"

#include <algorithm>
#include <cctype>

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

bool ends_with(const std::string &path, const std::string &suffix)
{
    if (suffix.size() > path.size())
        return false;
    return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string get_mime_type(const std::string &path)
{
    if (ends_with(path, ".html"))
        return "text/html";
    if (ends_with(path, ".css"))
        return "text/css";
    if (ends_with(path, ".js"))
        return "application/javascript";
    if (ends_with(path, ".png"))
        return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg"))
        return "image/jpeg";
    return "application/octet-stream";
}

std::optional<std::string> try_extract_message(std::string &buffer)
{
    size_t header_end = buffer.find("\r\n\r\n");
    if (header_end == std::string::npos)
        return std::nullopt;

    size_t content_length = 0;
    std::string cl_header = "Content-Length: ";
    size_t cl_pos = buffer.find(cl_header);
    if (cl_pos != std::string::npos && cl_pos < header_end)
    {
        size_t cl_end = buffer.find("\r\n", cl_pos);
        content_length = std::stoul(buffer.substr(cl_pos + cl_header.size(), cl_end - cl_pos - cl_header.size()));
    }

    size_t message_end = header_end + 4 + content_length;
    size_t body_received = buffer.size() - (header_end + 4);

    if (body_received < content_length)
        return std::nullopt;

    std::string message = buffer.substr(0, message_end);
    buffer.erase(0, message_end);
    return message;
}