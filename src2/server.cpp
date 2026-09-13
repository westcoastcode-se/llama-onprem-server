#include "common/tcp.hpp"
#include "common/request.hpp"
#include "common/protocol.hpp"

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

    static inline void signal_handler(const int signal) {
        shutdown_handler(signal);
    }

    struct ConnectedClient {
        // Socket to read and send data over
        unique_ptr<TcpSocket> socket;
        // Unique id for the client - used primarily for logging
        uint32_t id;

        friend std::ostream &operator<<(std::ostream &s, const ConnectedClient &c) {
            return s << "client(" << c.id << ")";
        }
    };

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
        // Thread running the client socket connection
        std::thread thread;
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
        WorkQueue work_queue;
        std::vector<unique_ptr<ClientThread> > client_threads;
        std::atomic_bool running{false};
        std::atomic_flag is_terminating = ATOMIC_FLAG_INIT;
        std::atomic_int next_id{};

        /**
         * Add a new client and spawn a new thread for it
         *
         * @param client The new client
         */
        void add_client_thread(unique_ptr<TcpSocket> &&client, const uint32_t id) {
            std::thread t(&Server::client_thread, this, ConnectedClient{std::move(client), id});
            client_threads.emplace_back(new ClientThread{.thread = std::move(t)});
        }

        /**
         * Start the server and accept incoming connections
         */
        void start() {
            log_info("starting server");
            const auto listener = TcpSocket::listen(config.address, config.port);
            running = true;
            while (running) {
                if (!listener->poll_incoming()) {
                    continue;
                }
                std::string client_ip;
                int client_port = 0;
                try {
                    auto client = listener->accept(&client_ip, &client_port);
                    const auto id = next_id++;
                    log_info("client(", id, ") | connected from ", client_ip, ":", client_port);
                    add_client_thread(std::move(client), id);
                } catch (const TcpSocket::accept_failed &_) {
                    // Ignore and continue
                    continue;
                }
            }
            log_info("server shutdown");
        }

        /**
         * Stop the server from running
         */
        void stop() {
            log_info("stopping server");
            if (is_terminating.test_and_set()) {
                log_error("received second interrupt, terminating immediately.");
                exit(1);
                return;
            }
            running = false;
        }

        /**
         * Send information on the server to the client
         *
         * @param client
         * @param buffer
         */
        template<class T>
        void send_server_info(const ConnectedClient &client, TBuffer<T> &buffer) {
        }

        /**
         * Authenticate the new client
         *
         * @param client
         * @param buffer
         */
        template<class T>
        void authenticate_client(const ConnectedClient &client, TBuffer<T> &buffer) {
            log_info(client, " | authenticating");
            const auto json = Request::read_request(client.socket, buffer);
            if (json.value("type", std::string_view()) != std::string_view("auth")) {
                throw auth_error{};
            }
            if (json.value("token", std::string_view()) != config.api_key) {
                throw auth_error{};
            }
            log_info(client, " | is now authenticated");
            buffer.clear();

            // Send information back to the client
            client.socket->pack_and_send(buffer, requests::ServerInfo{
                                             .model_path = config.model_path, .context = config.context,
                                         }.to_json());
        }

        /**
         * Handle a client request
         *
         * @param client
         * @param json
         */
        void handle_client_request(const ConnectedClient &client, const nlohmann::json &json) {
            if (json["type"] == std::string_view("chat")) {
                log_info(client, " | chat message=", json.value("message", std::string_view()));
            }
        }

        /**
         * Method called from a thread dedicated for the supplied client
         *
         * @param client Client
         */
        void client_thread(const ConnectedClient client) {
            try {
                // Validate client auth token
                TBuffer<HeapByteBuffer> buffer;
                authenticate_client(client, buffer);

                while (running) {
                    if (!client.socket->poll_incoming()) {
                        continue;
                    }
                    const auto json = Request::read_request(client.socket, buffer);
                    buffer.clear();
                    handle_client_request(client, json);
                }
            } catch (base_error &e) {
                log_error(client, " | unhandled error: ", e.what());
            }
            log_info(client, " | disconnected");
        }
    };
}

int main() {
    using callisto::ClientThread;
    using callisto::WorkQueue;
    using callisto::Server;
    using callisto::log_info;
    using callisto::log_error;

    Server server{.config = {.api_key = "SUPERSECRET"}};

    // Listen for interrupts
    callisto::shutdown_handler = [&server](int _) {
        server.stop();
    };
    signal(SIGINT, callisto::signal_handler);

    // Start the server
    // TODO: Add arguments
    // TODO: Allow running the client on the server
    // TODO: Allow running the server without allowing remote access
    try {
        server.start();
    } catch (const std::exception &e) {
        log_error("could not start server: ", e.what());
    }
    return 0;
}
