#pragma once

#include <stdexcept>
#include <string>

struct NotFound : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct Busy : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline std::string error_json(const std::string &code, const std::string &message)
{
    return std::string(R"({"error":{"code":")") + code + R"(","message":")" + message + R"("}})";
}
