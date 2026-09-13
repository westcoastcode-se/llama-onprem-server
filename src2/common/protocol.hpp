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

        [[nodiscard]] json to_json() const {
            return {{"role", role}, {"content", content}};
        }

        static ChatMessage from_json(const nlohmann::json &j) {
            ChatMessage msg;
            msg.role = j.value("role", "");
            msg.content = j.value("content", "");
            return msg;
        }
    };

    namespace requests {
        /**
         * Information from the server about the AI model and it's capabilities
         */
        struct ServerInfo {
            // Model path
            string model_path;
            // Context information
            int32_t context;

            [[nodiscard]] json to_json() const {
                return {
                    {"type", "server_info"},
                    {"model", model_path},
                    {"context", context}
                };
            }

            static ServerInfo from_json(const json &j) {
                ServerInfo msg;
                msg.model_path = j.value("model_path", "");
                msg.context = j.value("context", 0);
                return msg;
            }
        };

        /**
         * Response from the server of the AI's currently generated token
         */
        struct TokenResponse {
            // A piece of text
            string piece;
            // How much context is used
            int32_t context_used;

            [[nodiscard]] json to_json() const {
                return {
                    {"type", "token"},
                    {"piece", piece},
                    {"context_used", context_used}
                };
            }

            static TokenResponse from_json(const json &j) {
                TokenResponse msg;
                msg.piece = j.value("piece", "");
                msg.context_used = j.value("context_used", 0);
                return msg;
            }
        };

        /**
         * Response of the complete chat request
         */
        struct TokensDoneResponse {
            string response;
            int32_t context_used;

            [[nodiscard]] json to_json() const {
                return {
                    {"type", "tokens_done"},
                    {"response", response},
                    {"context_used", context_used}
                };
            }

            static TokensDoneResponse from_json(const json &j) {
                TokensDoneResponse msg;
                msg.response = j.value("response", "");
                msg.context_used = j.value("context_used", 0);
                return msg;
            }
        };

        /**
         * Response sent to the server if an error occurred while generating the response
         */
        struct ErrorResponse {
            string message;

            [[nodiscard]] json to_json() const {
                return {
                    {"type", "error"},
                    {"message", message}
                };
            }

            static ErrorResponse from_json(const json &j) {
                ErrorResponse msg;
                msg.message = j.value("message", "");
                return msg;
            }
        };

        struct AuthRequest {
            string token;

            [[nodiscard]] json to_json() const {
                return {
                    {"type", "auth"},
                    {"token", token}
                };
            }

            static AuthRequest from_json(const json &j) {
                AuthRequest msg;
                msg.token = j.value("token", "");
                return msg;
            }
        };
    }

    struct Requests {
        template<class T>
        static void abort_request(const unique_ptr<TcpSocket> &socket, TBuffer<T> &buffer) {
            const nlohmann::json request = {
                {"type", "abort"}
            };
            socket->pack_and_send(buffer, request);
        }
    };
}
