#pragma once

#include "std.hpp"

#include <ostream>
#include <sstream>
#include <utility>

enum class LogLevel : int
{
    Debug = 0,
    Info = 1,
    Error = 2,
    Off = 3,
};

class Logger
{
  public:
    static constexpr LogLevel LEVEL_DEBUG = LogLevel::Debug;
    static constexpr LogLevel LEVEL_INFO = LogLevel::Info;
    static constexpr LogLevel LEVEL_ERROR = LogLevel::Error;
    static constexpr LogLevel LEVEL_OFF = LogLevel::Off;

    [[nodiscard]] static bool is_level(LogLevel level);
    static void set_level(LogLevel level);
    static void write(LogLevel level, std::string_view text);
};

template <typename... Args> void log(LogLevel level, Args &&...args)
{
    if (!Logger::is_level(level))
    {
        return;
    }
    std::ostringstream out;
    (out << ... << std::forward<Args>(args));
    Logger::write(level, out.str());
}

template <typename... Args> void log_info(Args &&...args)
{
    log(LogLevel::Info, std::forward<Args>(args)...);
}

template <typename... Args> void log_debug(Args &&...args)
{
    log(LogLevel::Debug, std::forward<Args>(args)...);
}

template <typename... Args> void log_error(Args &&...args)
{
    log(LogLevel::Error, std::forward<Args>(args)...);
}
