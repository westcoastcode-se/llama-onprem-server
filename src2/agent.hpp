#pragma once

#include "common/tcp.hpp"
#include "common/request.hpp"
#include "common/protocol.hpp"
#include "common/console.hpp"

#include <string>
#include <string_view>
#include <iostream>
#include <atomic>

namespace callisto {
    /**
     * Lambda function definition used when an agent is running and we want to be able to
     * read an agents thinking process while it's happening
     */
    using AgentThinkStream = std::function<void(std::string_view piece)>;

    /**
     * Lambda function definition used to check if the current action should be aborted
     */
    using AbortFn = std::function<bool()>;

    /**
     * Agent
     */
    struct Agent {
        virtual ~Agent() = default;

        virtual bool is_aborting() = 0;

        virtual bool is_running() = 0;
    };

    /**
     * Remote agent that talks to the server
     */
    struct RemoteAgent : Agent {
        /**
         * Remote session
         */
        struct Session {
        };

        const unique_ptr<TcpSocket> &socket;
        std::atomic_bool requesting{false};
        std::atomic_bool aborting{false};
        int32_t session_id{0};

        explicit RemoteAgent(const unique_ptr<TcpSocket> &socket)
            : socket(socket) {
        }

        /**
         * @param message The chat message
         */
        void chat(TBuffer<HeapByteBuffer> &buffer, string_view message) {
            //std::cout << Colors::reset;
            //std::cout << Colors::gray << "thinking> ";
            // ...
            //std::cout << Colors::reset;

            // Send chat request
            socket->pack_and_send(buffer, requests::ChatRequest{
                                      .message = message, .session_id = session_id
                                  }.to_json());


            bool thinking = false;

            // Now wait for response data response. Expected responses are:
            // TokenResponse, TokensDoneResponse, TasksRequest, ErrorResponse
            while (!is_aborting()) {
                if (!socket->poll_incoming()) {
                    continue;
                }

                buffer.clear();
                const auto j = Request::read_request(socket, buffer);
                const auto type = j.value("type", string());

                if (type == requests::TokenResponse::type) {
                    const auto token = requests::TokenResponse::from_json(j);
                    if (!thinking) {
                        std::cout << Colors::gray << "thinking>" << Colors::reset;
                        thinking = true;
                    }
                    std::cout << Colors::gray << ' ' << token.piece << Colors::reset;
                } else if (type == requests::TokensDoneResponse::type) {
                    const auto token = requests::TokensDoneResponse::from_json(j);
                    std::cout << std::endl << Colors::yellow << "response> " << token.response;
                    std::cout << Colors::reset << std::endl;
                    break;
                } else if (type == requests::ErrorResponse::type) {
                    const auto err = requests::ErrorResponse::from_json(j);
                    // TODO: Add support for handling some errors, such as compaction required
                    break;
                }
            }

            std::cout << Colors::reset;
        }

        /**
         * Abort the current request
         */
        void abort() {
            if (requesting && !aborting) {
                TBuffer<StackByteBuffer<512> > buffer;
                socket->pack_and_send(buffer, requests::AbortRequest{}.to_json());
                aborting = true;
            }
        }

        bool is_aborting() final {
            return aborting;
        }

        bool is_running() final {
            return requesting;
        }
    };

    /**
     * A local agent
     */
    struct LocalAgent : Agent {
        bool is_aborting() final {
            return false;
        }

        bool is_running() final {
            return false;
        }
    };
}
