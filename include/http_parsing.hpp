#pragma once

#include <string>
#include <optional>

struct RequestLine
{
    std::string method;
    std::string path;
};

std::optional<RequestLine> parse_request_line(const std::string &request);
bool has_header(const std::string &request, const std::string &lowercase_name);
bool ends_with(const std::string &path, const std::string &suffix);
std::string get_mime_type(const std::string &path);
std::optional<std::string> try_extract_message(std::string &buffer);