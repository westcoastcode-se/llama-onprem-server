#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

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
