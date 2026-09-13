#pragma once

#include "request.hpp"
#include "tcp.hpp"

namespace callisto {
    struct Roles {
        // Response made by the AI
        static constexpr std::string_view assistant = "assistant";
        // Initial conversation message. Sets up things like tool configurations
        static constexpr std::string_view system = "system";
        // User made chat message to be sent from the client
        static constexpr std::string_view user = "user";
        // Role used for when a tool returns a response
        static constexpr std::string_view tool = "tool";
    };

    /**
     * History of the entire chat
     */
    struct ChatMessage {
        std::string role;
        std::string content;

        [[nodiscard]] nlohmann::json to_json() const {
            return {{"role", role}, {"content", content}};
        }

        static ChatMessage from_json(const nlohmann::json &j) {
            ChatMessage msg;
            msg.role = j.value("role", "");
            msg.content = j.value("content", "");
            return msg;
        }
    };

    /**
     * A full conversation
     */
    struct Conversation {
        std::string role;
        std::string content;

        [[nodiscard]] nlohmann::json to_json() const {
            return {{"role", role}, {"content", content}};
        }

        static ChatMessage from_json(const nlohmann::json &j) {
            ChatMessage msg;
            msg.role = j.value("role", "");
            msg.content = j.value("content", "");
            return msg;
        }
    };

    struct Requests {
        template<class T>
        static void server_info(const unique_ptr<TcpSocket> &socket, TBuffer<T> &buffer,
                                std::string_view model_path, int32_t context) {
            const nlohmann::json auth_request = {
                {"type", "server_info"},
                {"model", model_path},
                {"context", context}
            };
            socket->pack_and_send(buffer, auth_request);
        }

        template<class T>
        static void auth_request(const unique_ptr<TcpSocket> &socket, TBuffer<T> &buffer, const std::string_view token) {
            const nlohmann::json auth_request = {
                {"type", "auth"},
                {"token", token}
            };
            socket->pack_and_send(buffer, auth_request);
        }

        template<class T>
        static void abort_request(const unique_ptr<TcpSocket> &socket, TBuffer<T> &buffer) {
            const nlohmann::json request = {
                {"type", "abort"}
            };
            socket->pack_and_send(buffer, request);
        }
    };
}
