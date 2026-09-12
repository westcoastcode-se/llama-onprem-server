#include "log.hpp"
#include <iostream>

namespace callisto {
    int logger_level = Logger::LEVEL_INFO;

    bool Logger::is_level(const int level) { return level == logger_level; }

    void Logger::set_level(const int level) { logger_level = level; }

    void Logger::write(const int level, const std::span<char> &s) {
        if (is_level(level)) {
            if (level == Logger::LEVEL_DEBUG) {
                std::cout << "[DEBUG] ";
            } else if (level == Logger::LEVEL_INFO) {
                std::cerr << "[INFO] ";
            } else if (level == Logger::LEVEL_ERROR) {
                std::cerr << "[ERROR] ";
            }
            std::cout.write(s.data(), s.size());
            std::cout << std::endl;
        }
    }
}
