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

#include "common/errors.hpp"

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
     * Since this system is designed for on-prem installations, only one AI process is allowed
     * to run one request at a time (because, seriously, one AI process will take pretty much all the systems resources).
     *
     * So all work will be queued and processed one at a time.
     */
    struct WorkQueue {
        std::mutex mutex;
        std::queue<std::function<void()> > queue;

        /**
         * Add new work
         *
         * @param work Work
         */
        void add_work(std::function<void()> &&work) {
            std::lock_guard lock(mutex);
            queue.push(std::move(work));
        }
    };

    struct ClientThread {
        typedef std::unique_ptr<ClientThread> Ptr;

        std::thread thread;
        std::atomic_bool running;
    };

    /**
     * Represents a server that listens for incoming client connections and handles them.
     */
    struct Server {
        struct Config {
            // The server address
            std::string_view address = "0.0.0.0";
            // The server port
            int port = 8080;
            // Path to the model
            std::string model_path;
            // Allowed context size
            int context = 100000;
            // API-key
            std::string api_key;
        };

        Config config;
        std::unique_ptr<TcpSocket> listener;
        WorkQueue work_queue;
        std::vector<ClientThread::Ptr> client_threads;
        std::atomic_bool running{false};

        /**
         * Add a new client and spawn a new thread for it
         *
         * @param client The new client
         */
        void add_client_thread(TcpSocket::Ptr &&client) {
            std::thread t(&Server::client_thread, this, std::move(client));
            client_threads.emplace_back(new ClientThread{.thread = std::move(t), .running = true});
        }

        /**
         * Start the server and accept incoming connections
         *
         * @param config The server configuration
         */
        void start(const Config &config) {
            log_info("starting server");
            this->config = config;
            listener = TcpSocket::listen(config.address, config.port);
            running = true;
            while (running) {
                if (!listener->poll_incoming()) {
                    continue;
                }
                std::string client_ip;
                int client_port = 0;
                try {
                    auto client = listener->accept(&client_ip, &client_port);
                    log_info("accepted connection from {}:{}", client_ip, client_port);
                    add_client_thread(std::move(client));
                } catch (TcpSocket::accept_failed) {
                    // Ignore and continue
                    continue;
                }
            }
            listener = {};
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
         * Authenticate the new client
         *
         * @param client
         * @param buffer
         */
        void authenticate_client(const TcpSocket::Ptr &client, Buffer &buffer) {
            const auto r = client->read(buffer);
            const auto json = client_read_request(client, buffer);
            if (json["type"] != std::string_view("auth")) {
                throw auth_error{};
            }
            if (json["token"] != config.api_key) {
                throw auth_error{};
            }
            log_info("client authenticated");
            buffer.clear();
        }

        /**
         * Method called from a thread dedicated for the supplied client
         *
         * @param client Client
         */
        void client_thread(const TcpSocket::Ptr client) {
            try {
                // Validate client auth token
                Buffer buffer;
                authenticate_client(client, buffer);

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
            log_info("client disconnected");
        }
    };
}

int main() {
    using callisto::ClientThread;
    using callisto::WorkQueue;
    using callisto::Server;
    using callisto::log_info;
    using callisto::log_error;

    Server server;

    // Listen for interrupts
    callisto::shutdown_handler = [&server](int _) {
        if (callisto::is_terminating.test_and_set()) {
            log_error("Received second interrupt, terminating immediately.");
            exit(1);
            return;
        }
        server.stop();
    };
    signal(SIGINT, callisto::signal_handler);

    // Start the server
    try {
        server.start({.api_key = "SUPERSECRET"});
    } catch (const std::exception &_) {
        log_error("could not start server");
    }
    return 0;
}
