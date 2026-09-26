#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <latch>
#include <list>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using json = nlohmann::json;

using string = std::string;
using string_view = std::string_view;

template <typename T> using vector = std::vector<T>;
template <typename T> using deque = std::deque<T>;
template <typename T> using list = std::list<T>;

template <typename T> using span = std::span<T>;

template <typename T> using unique_ptr = std::unique_ptr<T>;
template <typename T> using shared_ptr = std::shared_ptr<T>;

using bytes = std::span<std::byte>;

using mutex = std::mutex;
using latch = std::latch;
using thread = std::jthread;
using condition_variable = std::condition_variable;

template <typename T> using optional = std::optional<T>;

// Counting latch whose count can rise. std::latch is fixed at construction.
class dynamic_latch
{
    std::mutex mutex_;
    std::condition_variable cv_;
    std::ptrdiff_t count_;

  public:
    explicit dynamic_latch(std::ptrdiff_t count) : count_(count)
    {
    }

    dynamic_latch(const dynamic_latch &) = delete;
    dynamic_latch &operator=(const dynamic_latch &) = delete;

    void add(std::ptrdiff_t n = 1)
    {
        std::lock_guard lock(mutex_);
        count_ += n;
    }

    void count_down(std::ptrdiff_t n = 1)
    {
        std::lock_guard lock(mutex_);
        count_ -= n;
        if (count_ <= 0)
        {
            cv_.notify_all();
        }
    }

    void wait()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return count_ <= 0; });
    }

    struct guard
    {
        dynamic_latch &latch;

        explicit guard(dynamic_latch &latch) : latch(latch)
        {
        }

        guard(const guard &) = delete;
        guard &operator=(const guard &) = delete;

        ~guard()
        {
            latch.count_down();
        }
    };
};
