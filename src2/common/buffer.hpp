#pragma once

#include <cstddef>
#include <cstring>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <string_view>

namespace callisto {
    /**
     * A reusable buffer for network communication.
     */
    class Buffer {
        // Underlying buffer
        std::vector<std::byte> buffer_;

    public:
        explicit Buffer(const std::size_t initial_capacity = 1024) {
            buffer_.reserve(initial_capacity);
        }

        /**
         *
         * @param sockfd The socket file descriptor to read from.
         * @param chunk_size The maximum number of bytes to read in each chunk.
         * @return The number of bytes read, or -1 if an error occurred.
         */
        ssize_t read(const int sockfd, const size_t chunk_size = 8096) {
            // Ensure buffer size
            if (buffer_.size() + chunk_size > buffer_.capacity()) {
                // Increase the size if necessary
                const size_t new_cap = std::max(buffer_.capacity() * 2, buffer_.size() + chunk_size);
                buffer_.reserve(new_cap);
            }

            const size_t old_size = buffer_.size();
            buffer_.resize(old_size + chunk_size);
            char* pp = (char*)&buffer_[0] + old_size;
            const ssize_t n = ::recv(sockfd, pp, chunk_size, 0);

            if (n > 0) {
                buffer_.resize(old_size + static_cast<size_t>(n));
            }

            return n;
        }

        void clear() {
            buffer_.clear();
        }

        [[nodiscard]] std::string_view data() const {
            return {reinterpret_cast<const char *>(buffer_.data()), buffer_.size()};
        }

        void put(const std::string_view str) {
            buffer_.reserve(buffer_.size() + str.length());
            const auto old_size = buffer_.size();
            buffer_.resize(old_size + static_cast<size_t>(str.length()));
            memcpy(buffer_.data() + old_size, str.data(), str.length());
        }
    };
}
