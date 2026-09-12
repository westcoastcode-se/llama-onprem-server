#pragma once

#include "buffer.hpp"
#include "log.hpp"
#include <cstring>
#include <unistd.h>
#include <memory>
#include <nlohmann/json.hpp>

#include "errors.hpp"

namespace callisto {
    /**
     * @class TcpSocket
     * @brief Represents a TCP socket for network communication.
     *
     * This class provides a basis for handling a TCP socket. It manages
     * the file descriptor associated with the socket and can be used to
     * extend functionality for creating, connecting, sending, and receiving
     * data over the network using the TCP protocol.
     *
     * The file descriptor is initialized to -1, indicating an invalid state.
     * Proper initialization and handling of the socket are expected in derived
     * or extended implementations.
     */
    class TcpSocket {
        int fd_ = -1;

    public:
        typedef std::unique_ptr<TcpSocket> Ptr;

        struct connect_error : base_error {
        };

        struct listen_error : base_error {
        };

        struct socket_closed : base_error {
        };

        struct accept_failed : base_error {
        };

        struct read_failed : base_error {
        };

        explicit TcpSocket(const int fd = -1) : fd_(fd) {
            if (fd_ >= 0) {
                const int flag = 1;
                setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            }
        }

        ~TcpSocket() {
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
        }

        /**
         *
         * @tparam T The buffer type
         * @param buffer The buffer that we can use when sending data
         * @param json The json body
         */
        template<class T>
        void pack_and_send(TBuffer<T> &buffer, const nlohmann::json &json) {
            const auto value = json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

            buffer.put("CALLISTO/1.0 ");
            buffer.put(std::to_string(value.length()));
            buffer.put(" ");
            buffer.put(value);
            send_all(buffer.data());
            buffer.clear();
        }

        /**
         * Connect to a server
         *
         * @param host The host we want to connect to
         * @param port The port
         * @return A socket, if connecting was successful
         */
        static Ptr connect(const std::string_view &host, const int port) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;

            // Port as a string
            char port_str[6];
            snprintf(port_str, sizeof(port_str), "%hu", port);

            addrinfo *res = nullptr;
            char host_str[256];
            snprintf(host_str, sizeof(host_str), "%s", host.data());
            int status = getaddrinfo(host_str, port_str, &hints, &res);
            if (status != 0 || !res) {
                log_error("failed to resolve address: ", status != 0 ? gai_strerror(status) : "unknown error");
                if (res) freeaddrinfo(res);
                throw connect_error{};
            }

            int fd = -1;
            for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
                fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (fd < 0) continue;

                if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
                    break; // connected successfully
                }

                ::close(fd);
                fd = -1;
            }

            freeaddrinfo(res);

            if (fd < 0) {
                log_error("failed do connect to ", host, ":", port, ". reason: ", strerror(errno));
                throw connect_error{};
            }

            return std::make_unique<TcpSocket>(fd);
        }

        static Ptr listen(const std::string_view host, const unsigned short port) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE;

            // Port as a string
            char port_str[6];
            snprintf(port_str, sizeof(port_str), "%hu", port);

            addrinfo *res = nullptr;
            if (host != "0.0.0.0") {
                char host_str[256];
                snprintf(host_str, sizeof(host_str), "%s", host.data());
                const auto status = getaddrinfo(host_str, port_str, &hints, &res);
                if (status != 0 || !res) {
                    log_error("failed to resolve address: ", status != 0 ? gai_strerror(status) : "unknown error");
                    if (res) freeaddrinfo(res);
                    throw listen_error{};
                }
            } else {
                const auto status = getaddrinfo(nullptr, port_str, &hints, &res);
                if (status != 0 || !res) {
                    log_error("failed to resolve address: ", status != 0 ? gai_strerror(status) : "unknown error");
                    if (res) freeaddrinfo(res);
                    throw listen_error{};
                }
            }

            const auto listen_fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (listen_fd_ < 0) {
                log_error("socket creation failed: ", strerror(errno));
                freeaddrinfo(res);
                throw listen_error{};
            }

            int opt = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if (::bind(listen_fd_, res->ai_addr, res->ai_addrlen) < 0) {
                log_error("bind failed: ", strerror(errno));
                freeaddrinfo(res);
                ::close(listen_fd_);
                throw listen_error{};
            }

            freeaddrinfo(res);
            if (::listen(listen_fd_, 128) < 0) {
                log_error("listen failed: ", strerror(errno));
                ::close(listen_fd_);
                throw listen_error{};
            }

            return std::make_unique<TcpSocket>(listen_fd_);
        }

        /**
         * Read data from the socket into the provided buffer.
         *
         * @param buffer The buffer to read data into.
         * @param chunk_size The maximum number of bytes to read in each chunk.
         * @return The number of bytes read.
         */
        template<class T>
        ssize_t read(TBuffer<T> &buffer, const size_t chunk_size = 8096) const {
            const auto n = buffer.read(fd_, chunk_size);
            if (n == -1) {
                throw read_failed{};
            }
            return n;
        }

        void send_all(const std::span<std::byte> data) const {
            if (fd_ < 0) throw socket_closed{};
            size_t total_sent = 0;
            auto len = data.size();
            while (total_sent < len) {
                const auto sent = ::send(fd_, data.data() + total_sent, len - total_sent, 0);
                if (sent < 0) {
                    if (errno == EINTR) continue;
                    throw socket_closed{};
                }
                if (sent == 0) throw socket_closed{};
                total_sent += static_cast<size_t>(sent);
            }
        }

        void close() {
            if (fd_) {
                ::close(fd_);
                fd_ = 0;
            }
        }

        [[nodiscard]] bool poll_incoming() const {
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;

            int poll_ret = poll(&pfd, 1, 500);
            if (poll_ret < 0) {
                if (errno == EINTR) return false;
                throw socket_closed{};
            }
            if (poll_ret == 0) {
                return false;
            }
            return true;
        }

        Ptr accept(std::string *client_ip, int *client_port) {
            if (fd_ < 0) throw socket_closed{};
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = ::accept(fd_, (sockaddr *) &client_addr, &addr_len);
            if (client_fd < 0) {
                throw accept_failed{};
            }
            if (client_ip) {
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
                *client_ip = ip_str;
            }
            if (client_port) {
                *client_port = ntohs(client_addr.sin_port);
            }
            return std::make_unique<TcpSocket>(client_fd);
        }
    };
}
