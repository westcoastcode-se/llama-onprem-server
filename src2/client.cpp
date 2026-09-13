#include "agent.hpp"

#include <functional>
#include <atomic>
#include <csignal>
#include <deque>
#include <thread>
#include <csignal>
#include <mutex>
#include <queue>
#include <iostream>

namespace callisto {
    static std::function<void(int)> shutdown_handler;

    static inline void signal_handler(const int signal) {
        shutdown_handler(signal);
    }

    /**
     * Remote connection to a server
     */
    struct RemoteConnection {
        struct Config {
            // The server address
            string_view address = "0.0.0.0";
            // The server port
            int port = 8080;
            // API-key
            string api_key;
        };

        Config config;
        std::unique_ptr<TcpSocket> socket;
        RemoteAgent agent{socket};
        std::atomic_bool running{false};
        std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

        /**
         * Authenticate
         *
         * @param buffer
         * @param string
         */
        void send_auth_request(TBuffer<HeapByteBuffer> &buffer, const std::string &string) {
            log_info("authenticating");
            socket->pack_and_send(buffer, requests::AuthRequest{.token = string}.to_json());

            // Wait for server_info response and print out information of it
            const auto server_info = Request::read_json<requests::ServerInfo>(socket, buffer);
            buffer.clear();

            std::cout << "Server: " << config.address << ":" << config.port << std::endl;
            std::cout << "Model: " << server_info.model_path << std::endl;
            std::cout << "Context: 0 / " << server_info.context << std::endl;
        }

        /**
         * Start the server and accept incoming connections
         *
         * @param config The server configuration
         */
        void start() {
            log_info("connecting to ", config.address, ":", config.port);
            socket = TcpSocket::connect(config.address, config.port);

            TBuffer<HeapByteBuffer> buffer;
            try {
                send_auth_request(buffer, config.api_key);
            } catch (const base_error &e) {
                log_error("failed to authenticate: ", e.what());
                return;
            }

            running = true;
            while (running) {
                // Output
                std::cout << "> ";

                // Read the chat message
                std::string chat_message;
                std::getline(std::cin, chat_message);

                // Send message to agent
                agent.chat(buffer, chat_message);
            }
            socket = {};
        }

        /**
         * Interrupt the current action. If no action is running then shut down client down instead
         */
        void interrupt() {
            if (agent.is_running()) {
                agent.abort();
            } else {
                stop();
            }
        }

        /**
         * Stop the server from running
         */
        void stop() {
            if (is_terminating.test_and_set()) {
                log_error("received second stop action... terminating immediately.");
                exit(1);
                return;
            }
            running = false;
        }
    };
}

int main() {
    using namespace callisto;
    RemoteConnection client{.config = {"127.0.0.1", 8080, "SUPERSECRET"}};

    // Listen for interrupts
    shutdown_handler = [&client](int _) {
        client.interrupt();
    };
    signal(SIGINT, signal_handler);

    try {
        client.start();
    } catch (const base_error &e) {
        log_error(e.what());
    }
    std::cout.flush();
    return 0;
}
