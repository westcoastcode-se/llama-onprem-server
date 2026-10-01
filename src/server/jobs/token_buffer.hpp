#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/**
 * Thread-safe bridge: LLM producer pushes token pieces; HTTP consumer pulls them.
 * Distinguishes "no data yet" from EOF via wait + done flag.
 */
class TokenBuffer
{
  public:
    void push(std::string piece)
    {
        {
            std::lock_guard lock(mutex_);
            if (cancelled_ || done_)
            {
                return;
            }
            pieces_.push_back(std::move(piece));
        }
        cv_.notify_all();
    }

    void set_done()
    {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
    }

    void cancel()
    {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
            done_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool is_done() const
    {
        std::lock_guard lock(mutex_);
        return done_ && pieces_.empty();
    }

    [[nodiscard]] bool is_cancelled() const
    {
        std::lock_guard lock(mutex_);
        return cancelled_;
    }

    /**
     * Block until a piece is available or generation finished.
     *
     * @return nullopt when EOF (done and empty); empty string should not be returned for EOF.
     */
    std::optional<std::string> wait_pull()
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !pieces_.empty() || done_ || cancelled_; });
        if (!pieces_.empty())
        {
            std::string piece = std::move(pieces_.front());
            pieces_.pop_front();
            return piece;
        }
        return std::nullopt;
    }

    /**
     * Non-blocking pull of all currently available pieces.
     * @param out_done set true when generation finished and buffer drained.
     */
    std::vector<std::string> pull_available(bool &out_done)
    {
        std::lock_guard lock(mutex_);
        std::vector<std::string> out;
        out.reserve(pieces_.size());
        while (!pieces_.empty())
        {
            out.push_back(std::move(pieces_.front()));
            pieces_.pop_front();
        }
        out_done = done_ && pieces_.empty();
        return out;
    }

    void set_full_result(std::string text)
    {
        std::lock_guard lock(mutex_);
        full_result_ = std::move(text);
    }

    [[nodiscard]] std::string full_result() const
    {
        std::lock_guard lock(mutex_);
        return full_result_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> pieces_;
    std::string full_result_;
    bool done_ = false;
    bool cancelled_ = false;
};
