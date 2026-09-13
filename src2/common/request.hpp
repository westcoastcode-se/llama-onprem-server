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
         * Header: <VERSION> <LENGTH:10><BODY>
         *
         * The length if the complete size of both the header and the body
         */
        struct Headers {
            // The header version
            static constexpr std::string VERSION = "CALLISTO/1";
            // Header is the size of the "<VERSION> <char:10>" containing the size of the message
            static constexpr int32_t HEADER_LENGTH = (VERSION.length()) + 1 + 10;
        };

        /**
         *
         */
        struct request_error : base_error {
            [[nodiscard]] const char *what() const noexcept override {
                return "request_error";
            }
        };

        /**
         *
         */
        struct invalid_request : request_error {
            [[nodiscard]] const char *what() const noexcept final {
                return "invalid_request";
            }
        };

        /**
         *
         */
        struct invalid_version : base_error {
            [[nodiscard]] const char *what() const noexcept final {
                return "invalid_version";
            }
        };

        /**
         *
         */
        struct parse_error : base_error {
            [[nodiscard]] const char *what() const noexcept final {
                return "parse_error";
            }
        };

        /**
         * Parse a new request from the supplied buffer
         *
         * @param buffer The buffer
         * @return the lenghth of the request
         */
        template<class T>
        static uint32_t validate_and_get_length(const TBuffer<T> &buffer) {
            const auto parts = split2(buffer.string());
            if (!parts) {
                throw invalid_request{};
            }

            auto [version, length] = *parts;
            if (version != Headers::VERSION) {
                throw invalid_version{};
            }

            // Figure out the actual body length
            uint32_t json_length;
            std::ispanstream s(std::span(length.data(), length.size()));
            s >> json_length;
            return json_length;
        }

        /**
         * @param socket
         * @param buffer
         * @return
         */
        template<class BUFFER>
        static json read_request(const unique_ptr<TcpSocket> &socket, TBuffer<BUFFER> &buffer) {
            // Clear the buffer in preparation for the request
            buffer.clear();

            // Read the header
            if (socket->read(buffer, Headers::HEADER_LENGTH) != Headers::HEADER_LENGTH) {
                throw invalid_request{};
            }
            // Read the body
            const auto body_length = validate_and_get_length(buffer);
            buffer.clear();
            if (socket->read(buffer, body_length) != body_length) {
                throw invalid_request{};
            }

            const std::string_view json = buffer.string();
            log_debug("Received json: ", json);
            return nlohmann::json::parse(json, nullptr, false, true);
        }

        /**
         * @tparam T The model type
         * @tparam BUFFER The buffer implementation
         * @param socket Socket
         * @param buffer Buffer
         * @return The deserialized json object
         */
        template<class T, class BUFFER>
        static T read_json(const unique_ptr<TcpSocket> &socket, TBuffer<BUFFER> &buffer) {
            const auto j = read_request(socket, buffer);
            if (j["type"] != T::type) {
                throw invalid_request{};
            }
            return T::from_json(j);
        }

        static std::optional<std::tuple<std::string_view, std::string_view> > split2(
            const std::string_view sv, const char delim = ' ') {
            const size_t pos1 = sv.find(delim);
            if (pos1 == std::string_view::npos) return std::nullopt;

            return std::tuple{
                sv.substr(0, pos1), // del 1
                sv.substr(pos1 + 1)
            };
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
