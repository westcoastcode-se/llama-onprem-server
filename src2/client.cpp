#include "common/tcp.hpp"
#include "common/request.hpp"

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
    static std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;

    static inline void signal_handler(const int signal) {
        if (is_terminating.test_and_set()) {
            // in case it hangs, we can force terminate the server by hitting Ctrl+C twice
            // this is for better developer experience, we can remove when the server is stable enough
            fprintf(stderr, "Received second interrupt, terminating immediately.\n");
            exit(1);
        }

        shutdown_handler(signal);
    }

    /**
     * Represents a client that communicate with an the server
     */
    struct Client {
        struct Config {
            // The server address
            std::string_view address = "0.0.0.0";
            // The server port
            int port = 8080;
            // API-key
            std::string api_key;
        };

        Config config;
        std::unique_ptr<TcpSocket> socket;
        std::atomic_bool running{false};

        /**
         * Authenticate
         *
         * @param buffer
         * @param string
         */
        void send_auth_request(Buffer & buffer, const std::string & string) {
            const nlohmann::json auth_request = {
                {"type", "auth"},
                {"token", config.api_key}
            };
            socket->send_json(buffer, auth_request);
            buffer.clear();

            // Wait for server_info response and print out information of it
            const nlohmann::json server_info = Request::read_request(socket, buffer);
            std::cout << "Server: " << config.address << ":" << config.port << std::endl;
            std::cout << "Model: " << server_info["model"].get<std::string>() << std::endl;
            std::cout << "Context: 0 / " << server_info["context"].get<int32_t>() << std::endl;
        }

        /**
         * Start the server and accept incoming connections
         *
         * @param config The server configuration
         */
        void start(const Config &config) {
            log_info("starting client");
            this->config = config;
            socket = TcpSocket::connect(config.address, config.port);

            log_info("authenticating");
            Buffer buffer;
            send_auth_request(buffer, config.api_key);

            running = true;
            while (running) {
                std::cout << "> ";
                std::cin.get();
            }
            socket = {};
        }

        /**
         * Stop the server from running
         */
        void stop() {
            running = false;
        }

        /**
         * Method called from a thread dedicated for the supplied client
         *
         * @param client Client
         */
        void client_thread(const TcpSocket::Ptr client) {
            Buffer buffer;
            try {
                while (running) {
                    if (!client->poll_incoming()) {
                        continue;
                    }
                    const auto r = client->read(buffer);
                    Request::read_request(client, buffer);
                    buffer.clear();
                }
            } catch (base_error &e) {
                log_error("failed to parse request: ", e.what());
            }
        }
    };
}

int main() {
    using callisto::Client;
    using callisto::log_info;
    using callisto::log_error;

    Client client;

    // Listen for interrupts
    callisto::shutdown_handler = [&client](int _) {
        if (callisto::is_terminating.test_and_set()) {
            log_error("Received second interrupt, terminating immediately.");
            exit(1);
            return;
        }
        client.stop();
    };
    signal(SIGINT, callisto::signal_handler);

    client.start({"127.0.0.1", 8080, "SUPERSECRET"});
    return 0;
}
