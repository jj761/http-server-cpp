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

    size_t line_end = request.find("\r\n", second_space + 1);
    std::string version = (line_end == std::string::npos)
                              ? request.substr(second_space + 1)
                              : request.substr(second_space + 1, line_end - second_space - 1);

    RequestLine result;
    result.method = request.substr(0, first_space);
    result.path = path;
    result.version = version;
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

std::optional<std::string> get_header_value(const std::string &request, const std::string &lowercase_name)
{
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = (header_end == std::string::npos)
                              ? request
                              : request.substr(0, header_end);

    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });

    std::string marker = "\r\n" + lowercase_name + ":";
    size_t pos = headers.find(marker);
    if (pos == std::string::npos)
        return std::nullopt;

    size_t value_start = pos + marker.size();
    size_t value_end = headers.find("\r\n", value_start);
    if (value_end == std::string::npos)
        value_end = headers.size();

    while (value_start < value_end && std::isspace(static_cast<unsigned char>(headers[value_start])))
        ++value_start;
    while (value_end > value_start && std::isspace(static_cast<unsigned char>(headers[value_end - 1])))
        --value_end;

    return headers.substr(value_start, value_end - value_start);
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
    auto cl_value = get_header_value(buffer, "content-length");
    if (cl_value)
    {
        try
        {
            size_t parsed_chars = 0;
            unsigned long parsed = std::stoul(*cl_value, &parsed_chars);
            if (parsed_chars != cl_value->size())
                return std::nullopt; // trailing garbage, e.g. "5abc" -- treat as malformed
            content_length = parsed;
        }
        catch (const std::exception &)
        {
            return std::nullopt; // non-numeric value -- treat as malformed, don't crash
        }
    }

    size_t message_end = header_end + 4 + content_length;
    size_t body_received = buffer.size() - (header_end + 4);

    if (body_received < content_length)
        return std::nullopt;

    std::string message = buffer.substr(0, message_end);
    buffer.erase(0, message_end);
    return message;
}