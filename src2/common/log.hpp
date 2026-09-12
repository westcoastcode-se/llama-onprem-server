#pragma once

#include <array>
#include <cassert>
#include <span>
#include <spanstream>

namespace callisto {


    /**
     * Logger that we can use
     */
    class Logger {
    public:
        static constexpr int LEVEL_INFO = 0;
        static constexpr int LEVEL_ERROR = 1;
        static constexpr int LEVEL_OFF = 2;

        /**
         * @return true if the log is quiet
         */
        static bool is_level(int level);

        /**
         * @param level The log level
         */
        static void set_level(int level);

        /**
         * @param s The text to write
         */
        static void write(int level, const std::span<char>& s);

        /**
         * @tparam Str
         * @param ss
         * @param arg
         */
        template <typename Str> static void log_trait(std::ospanstream& ss, const Str& arg) { ss << arg; }

        /**
         * @tparam Str
         * @param ss
         * @param arg
         * @param args
         */
        template <typename S, typename... Str> static void log_trait(std::ospanstream& ss, const S& arg, Str&&... args) {
            ss << arg;
            log_trait(ss, std::forward<Str>(args)...);
        }
    };

    /**
     * @tparam Str
     * @param arg
     * @param args
     */
    template <typename S, typename... Str> static void log(const int level, const S& arg, Str&&... args) {
        if (Logger::is_level(level)) {
            std::array<char, 1024> buffer; // NOLINT(*-pro-type-member-init)
            std::ospanstream ss(buffer);
            Logger::log_trait(ss, arg, args...);
            Logger::write(level, ss.span());
            ss.seekp(0);
        }
    }

    /**
     * @tparam Str
     * @param arg
     * @param args
     */
    template <typename S, typename... Str> static void log_info(const S& arg, Str&&... args) {
        if (Logger::is_level(Logger::LEVEL_INFO)) {
            std::array<char, 1024> buffer; // NOLINT(*-pro-type-member-init)
            std::ospanstream ss(buffer);
            Logger::log_trait(ss, arg, args...);
            Logger::write(Logger::LEVEL_INFO, ss.span());
            ss.seekp(0);
        }
    }

    /**
     * @tparam Str
     * @param arg
     * @param args
     */
    template <typename S, typename... Str> static void log_error(const S& arg, Str&&... args) {
        if (Logger::is_level(Logger::LEVEL_ERROR)) {
            std::array<char, 1024> buffer; // NOLINT(*-pro-type-member-init)
            std::ospanstream ss(buffer);
            Logger::log_trait(ss, arg, args...);
            Logger::write(Logger::LEVEL_ERROR, ss.span());
            ss.seekp(0);
        }
    }


}
