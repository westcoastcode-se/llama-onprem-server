#pragma once

#include "buffer.hpp"

#include <optional>
#include <string_view>

#include "errors.hpp"

namespace callisto {
    /**
     *
     */
    struct Request {
        /**
         * Header: <VERSION> <API_KEY> <LENGTH> <BODY>
         *
         * The length if the complete size of both the header and the body
         */
        struct Headers {
            static constexpr std::string VERSION = "CALLISTO/1.0";
        };

        /**
         *
         */
        struct invalid_request : base_error {
            const char *what() const noexcept {
                return "invalid header";
            }
        };

        /**
         *
         */
        struct invalid_version : base_error {
            const char *what() const noexcept {
                return "invalid version";
            }
        };

        /**
         *
         */
        struct parse_error : base_error {
            const char *what() const noexcept {
                return "parse error";
            }
        };

        /**
         * Parse a new request from the supplied buffer
         *
         * @param buffer The buffer
         * @return the lenghth of the request
         */
        static std::tuple<uint32_t, uint32_t> validate_and_get_length(const Buffer &buffer) {
            const auto parts = split3(buffer.data());
            if (!parts) {
                throw invalid_request{};
            }

            auto [version, length, json] = *parts;
            if (version != Headers::VERSION) {
                throw invalid_version{};
            }

            // Figure out the actual body length
            uint32_t json_length;
            std::ispanstream s(std::span(length.data(), length.size()));
            s >> json_length;

            const uint32_t json_start_index = json.data() - buffer.data().data();
            return {json_length, json_start_index};
        }

        static std::optional<std::tuple<std::string_view, std::string_view, std::string_view> > split3(
            const std::string_view sv, const char delim = ' ') {
            const size_t pos1 = sv.find(delim);
            if (pos1 == std::string_view::npos) return std::nullopt;

            const size_t pos2 = sv.find(delim, pos1 + 1);
            if (pos2 == std::string_view::npos) return std::nullopt;

            return std::tuple{
                sv.substr(0, pos1), // del 1
                sv.substr(pos1 + 1, pos2 - pos1 - 1), // del 2
                sv.substr(pos2 + 1) // del 3
            };
        }

        static std::optional<std::tuple<std::string_view, std::string_view, std::string_view, std::string_view> >
        split4(const std::string_view sv, const char delim = ' ') {
            const size_t pos1 = sv.find(delim);
            if (pos1 == std::string_view::npos) return std::nullopt;

            const size_t pos2 = sv.find(delim, pos1 + 1);
            if (pos2 == std::string_view::npos) return std::nullopt;

            const size_t pos3 = sv.find(delim, pos2 + 1);
            if (pos3 == std::string_view::npos) return std::nullopt;

            return std::tuple{
                sv.substr(0, pos1), // del 1
                sv.substr(pos1 + 1, pos2 - pos1 - 1), // del 2
                sv.substr(pos2 + 1, pos3 - pos2 - 1), // del 3
                sv.substr(pos3 + 1) // del 3
            };
        }
    };
}
