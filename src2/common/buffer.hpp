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
#include <span>
#include <string_view>

namespace callisto {
    /**
     * @param s The span
     * @return A string view variant
     */
    std::string_view to_string(std::span<std::string_view::value_type> s) {
        return {s.data(), s.size()};
    }

    /**
     * Template wrapper over different types of buffers
     *
     * @tparam T The buffer implementation
     */
    template<class T>
    struct TBuffer {
        T impl{};

        /**
         * Put the supplied value into this buffer
         *
         * @tparam U The value type
         * @param value The value
         */
        template<typename U>
        void put(const U &value) {
            impl.put(value);
        }

        /**
         * @return Bytes in this buffer
         */
        std::span<std::byte> data() {
            return impl.data();
        }

        /**
         * @return Bytes in this buffer
         */
        [[nodiscard]] std::span<std::byte> data() const {
            return impl.data();
        }

        /**
         * @return The memory as a string
         */
        [[nodiscard]] std::string_view string() const {
            return impl.string();
        }

        /**
         * Clear this buffer so that we can reuse it
         */
        void clear() {
            impl.clear();
        }


        /**
         * Read data from a socket and put the result into this buffer
         *
         * @param sockfd The socket file descriptor to read from.
         * @param chunk_size The maximum number of bytes to read in each chunk.
         * @return The number of bytes read, or -1 if an error occurred.
         */
        ssize_t read(const int sockfd, const size_t chunk_size = 8096) {
            impl.ensure_capacity(chunk_size);
            const auto old_size = data().size();
            impl.resize(chunk_size);
            auto ptr = data();
            const auto pp = reinterpret_cast<char *>(&ptr[0]) + old_size;
            const auto n = ::recv(sockfd, pp, chunk_size, 0);
            if (n > 0) {
                impl.resize(old_size + n);
            }
            return n;
        }
    };

    /**
     * A buffer on the stack
     *
     * @tparam N Maximum number of bytes allowed to be packed in this buffer
     */
    template<size_t N>
    struct StackByteBuffer {
        std::byte bytes[N];
        size_t size = 0;

        void put(std::string_view str) {
            if (str.length() + size > N) {
                // TODO use better exception here!
                throw std::exception();
            }
            memcpy(bytes, str.data(), str.length());
            size += str.length();
        }

        std::span<std::byte> data() {
            return {bytes, size};
        }

        [[nodiscard]] std::string_view string() const {
            return {(std::string_view::value_type *) bytes, size};
        }

        [[nodiscard]] std::span<const std::byte> data() const {
            return {bytes, size};
        }

        void clear() {
            size = 0;
        }

        void resize(const size_t new_size) {
            if (new_size < 0 || new_size > N) {
                // TODO use better exception here!
                throw std::exception();
            }
            size = new_size;
        }

        void ensure_capacity(const size_t chunk_size) {
            if (size + chunk_size > N) {
                // TODO use better exception here!
                throw std::exception();
            }
        }
    };

    /**
     * A reusable buffer for network communication.
     */
    class HeapByteBuffer {
        // Underlying buffer
        std::vector<std::byte> buffer_;

    public:
        explicit HeapByteBuffer(const std::size_t initial_capacity = 1024) {
            buffer_.reserve(initial_capacity);
        }


        void clear() {
            buffer_.clear();
        }

        std::span<std::byte> data() {
            if (buffer_.empty()) {
                return {};
            }
            return {(&buffer_[0]), buffer_.size()};
        }

        [[nodiscard]] std::span<const std::byte> data() const {
            if (buffer_.empty()) {
                return {};
            }
            return {(&buffer_[0]), buffer_.size()};
        }

        [[nodiscard]] std::string_view string() const {
            if (buffer_.empty()) {
                return {};
            }
            return {reinterpret_cast<const char *>(buffer_.data()), buffer_.size()};
        }

        void put(const std::string_view str) {
            buffer_.reserve(buffer_.size() + str.length());
            const auto old_size = buffer_.size();
            buffer_.resize(old_size + str.length());
            memcpy(buffer_.data() + old_size, str.data(), str.length());
        }

        void resize(const size_t new_size) {
            buffer_.resize(new_size);
        }

        void ensure_capacity(const size_t chunk_size) {
            if (buffer_.size() + chunk_size > buffer_.capacity()) {
                const size_t new_cap = std::max(buffer_.capacity() * 2, buffer_.size() + chunk_size);
                buffer_.reserve(new_cap);
            }
        }
    };
}
