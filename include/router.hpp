#pragma once

#include <string>

struct ProcessResult
{
    std::string response;
    bool should_close;
};

ProcessResult process_request(const std::string &raw, const std::string &public_dir);