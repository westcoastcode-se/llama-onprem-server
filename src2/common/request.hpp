#pragma once

#include "buffer.hpp"
#include "errors.hpp"
#include "log.hpp"

#include <optional>
#include <string_view>

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
        template<class T>
        static std::tuple<uint32_t, uint32_t> validate_and_get_length(const TBuffer<T> &buffer) {
            const auto parts = split3(buffer.string());
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

            const uint32_t json_start_index = json.data() - buffer.string().data();
            return {json_length, json_start_index};
        }

        /**
         * @param socket
         * @param buffer
         * @return
         */
        template<class T>
        static nlohmann::json read_request(const TcpSocket::Ptr &socket, TBuffer<T> &buffer) {
            socket->read(buffer);
            const auto [length, json_offset] = validate_and_get_length(buffer);
            const auto buffer_data = buffer.string();
            if (buffer_data.size() < length) {
                // Read the rest of the data
                const auto n = socket->read(buffer, length - buffer_data.size());
                if (n != length) {
                    throw TcpSocket::read_failed{};
                }
            }

            const std::string_view json = buffer.string().substr(json_offset);
            log_debug("Received json: ", json);
            return nlohmann::json::parse(json, nullptr, false, true);
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
    };
}
