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
            const nlohmann::json auth_request = {
                {"type", "auth"},
                {"token", config.api_key}
            };
            socket->send_json(buffer, auth_request);
            running = true;

            socket = {};
        }

        /**
         * Stop the server from running
         */
        void stop() {
            running = false;
        }

        /**
         *
         * @param buffer The buffer to read data from
         */
        nlohmann::json client_read_request(const TcpSocket::Ptr &client, Buffer &buffer) {
            const auto [length, json_offset] = Request::validate_and_get_length(buffer);
            if (buffer.data().length() < length) {
                // Read the rest of the data
                const auto n = client->read(buffer, length - buffer.data().length());
                if (n != length) {
                    throw TcpSocket::read_failed{};
                }
            }

            const std::string_view json = buffer.data().substr(json_offset);
            log_info("Received json: ", json);
            return nlohmann::json::parse(json, nullptr, false, true);
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
                    client_read_request(client, buffer);
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
