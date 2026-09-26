#pragma once

#include <format>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <memory>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <nlohmann/json.hpp>

class TcpSocket {
public:
    explicit TcpSocket(int fd = -1) : fd_(fd) {
        if (fd_ >= 0) {
            int flag = 1;
            setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        }
    }

    ~TcpSocket() {
        close();
    }

    TcpSocket(const TcpSocket &) = delete;
    TcpSocket & operator=(const TcpSocket &) = delete;

    TcpSocket(TcpSocket && other) noexcept : fd_(other.fd_), read_buf_(std::move(other.read_buf_)) {
        other.fd_ = -1;
    }

    TcpSocket & operator=(TcpSocket && other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            read_buf_ = std::move(other.read_buf_);
            other.fd_ = -1;
        }
        return *this;
    }

    bool is_valid() const { return fd_ >= 0; }
    int native_handle() const { return fd_; }

    bool set_timeout(int seconds) {
        if (fd_ < 0) return false;
        struct timeval tv{};
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        read_buf_.clear();
    }

    bool send_all(const void * data, size_t len) {
        if (fd_ < 0) return false;
        const char * ptr = static_cast<const char *>(data);
        size_t total_sent = 0;
        while (total_sent < len) {
            ssize_t sent = ::send(fd_, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (sent == 0) return false;
            total_sent += static_cast<size_t>(sent);
        }
        return true;
    }

    bool send_all(std::span<const char> data) {
        return send_all(data.data(), data.size());
    }

    bool send_line(std::string_view str) {
        if (str.empty()) {
            return send_all("\n", 1);
        }
        if (str.ends_with('\n')) {
            return send_all(str.data(), str.size());
        }
        return send_all(str.data(), str.size()) && send_all("\n", 1);
    }

    bool send_json(const nlohmann::json & j) {
        try {
            return send_line(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        } catch (...) {
            return false;
        }
    }

    bool read_line(std::string & out_line) {
        if (fd_ < 0) return false;
        while (true) {
            size_t newline_pos = read_buf_.find('\n');
            if (newline_pos != std::string::npos) {
                out_line = read_buf_.substr(0, newline_pos);
                read_buf_.erase(0, newline_pos + 1);
                if (!out_line.empty() && out_line.back() == '\r') {
                    out_line.pop_back();
                }
                return true;
            }

            char chunk[4096];
            ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) {
                if (!read_buf_.empty()) {
                    out_line = read_buf_;
                    read_buf_.clear();
                    if (!out_line.empty() && out_line.back() == '\r') {
                        out_line.pop_back();
                    }
                    return true;
                }
                return false;
            }
            read_buf_.append(chunk, static_cast<size_t>(n));
        }
    }

    bool read_json(nlohmann::json & out_json) {
        std::string line;
        if (!read_line(line)) return false;
        try {
            out_json = nlohmann::json::parse(line, nullptr, false, true);
            return !out_json.is_discarded();
        } catch (...) {
            return false;
        }
    }

private:
    int fd_ = -1;
    std::string read_buf_;
};

struct AddrinfoDeleter
{
    void operator()(addrinfo *info) const noexcept
    {
        freeaddrinfo(info);
    }
};

using AddrinfoPtr = std::unique_ptr<addrinfo, AddrinfoDeleter>;

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer() { close(); }

    TcpServer(const TcpServer &) = delete;
    TcpServer & operator=(const TcpServer &) = delete;

    TcpServer(TcpServer && other) noexcept : listen_fd_(other.listen_fd_) {
        other.listen_fd_ = -1;
    }

    TcpServer & operator=(TcpServer && other) noexcept {
        if (this != &other) {
            close();
            listen_fd_ = other.listen_fd_;
            other.listen_fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] bool listen(const std::string & host, int port, std::string & error) {
        close();

        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        const std::string port_str = std::to_string(port);
        const char * host_ptr = (host.empty() || host == "0.0.0.0") ? nullptr : host.c_str();

        addrinfo *raw = nullptr;
        const int status = getaddrinfo(host_ptr, port_str.c_str(), &hints, &raw);
        const AddrinfoPtr resolved(raw);
        if (status != 0 || !resolved) {
            error = std::format("failed to resolve address: {}", status != 0 ? gai_strerror(status) : "unknown error");
            return false;
        }

        listen_fd_ = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        if (listen_fd_ < 0) {
            error = std::format("socket creation failed: {}", strerror(errno));
            return false;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (::bind(listen_fd_, resolved->ai_addr, resolved->ai_addrlen) < 0) {
            error = std::format("bind failed: {}", strerror(errno));
            close();
            return false;
        }

        if (::listen(listen_fd_, 128) < 0) {
            error = std::format("listen failed: {}", strerror(errno));
            close();
            return false;
        }

        return true;
    }

    std::unique_ptr<TcpSocket> accept(std::string * client_ip = nullptr, int * client_port = nullptr) {
        if (listen_fd_ < 0) return nullptr;
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &addr_len);
        if (client_fd < 0) {
            return nullptr;
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

    void close() {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    bool is_valid() const { return listen_fd_ >= 0; }
    int native_handle() const { return listen_fd_; }

private:
    int listen_fd_ = -1;
};

class TcpClient {
public:
    [[nodiscard]] static std::unique_ptr<TcpSocket> connect(const std::string & host, int port, std::string & error) {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        const std::string port_str = std::to_string(port);
        addrinfo *raw = nullptr;
        const int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &raw);
        const AddrinfoPtr resolved(raw);
        if (status != 0 || !resolved) {
            error = std::format("failed to resolve host: {}", status != 0 ? gai_strerror(status) : "unknown error");
            return nullptr;
        }

        int fd = -1;
        for (const addrinfo *entry = resolved.get(); entry != nullptr; entry = entry->ai_next) {
            fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (fd < 0) continue;

            if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
                break;
            }

            ::close(fd);
            fd = -1;
        }

        if (fd < 0) {
            error = std::format("connect failed: {}", strerror(errno));
            return nullptr;
        }

        return std::make_unique<TcpSocket>(fd);
    }
};
