#include "log.hpp"

#include <print>
#include <utility>

namespace
{
LogLevel logger_level = LogLevel::Info;

constexpr std::string_view level_tag(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Off:
        return "";
    }
    std::unreachable();
}
} // namespace

bool Logger::is_level(const LogLevel level)
{
    return std::to_underlying(level) >= std::to_underlying(logger_level);
}

void Logger::set_level(const LogLevel level)
{
    logger_level = level;
}

void Logger::write(const LogLevel level, const std::string_view text)
{
    if (!is_level(level) || level == LogLevel::Off)
    {
        return;
    }
    std::println("[{}] {}", level_tag(level), text);
}
