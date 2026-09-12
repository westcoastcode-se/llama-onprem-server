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

        /**
         * @param message The chat message
         */
        void chat(std::string_view message) {
            std::cout << Colors::reset;
            std::cout << Colors::gray << "thinking> ";
            // ...
            std::cout << Colors::reset;
        }

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

        const TcpSocket::Ptr &socket;
        std::atomic_bool requesting{false};
        std::atomic_bool aborting{false};

        explicit RemoteAgent(const TcpSocket::Ptr &socket)
            : socket(socket) {
        }

        /**
         * Abort the current request
         */
        void abort() {
            if (requesting && !aborting) {
                TBuffer<StackByteBuffer<512> > buffer;
                Requests::abort_request(socket, buffer);
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
