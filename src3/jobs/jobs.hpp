#pragma once

#include "api/messages.hpp"
#include "jobs/token_buffer.hpp"
#include "llm/llm_engine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class TaskState
{
    Queued,
    Running,
    Done,
    Error,
    Cancelled,
};

inline const char *to_string(const TaskState s)
{
    switch (s)
    {
    case TaskState::Queued:
        return "queued";
    case TaskState::Running:
        return "running";
    case TaskState::Done:
        return "done";
    case TaskState::Error:
        return "error";
    case TaskState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

struct Task
{
    typedef string Key;

    Key key;
    MessagesRequest request;
    shared_ptr<TokenBuffer> buffer = std::make_shared<TokenBuffer>();
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished_at{};

    // Optional hook when task reaches a terminal state (done/error/cancelled).
    std::function<void(const Task &)> on_finished;

    mutable std::mutex mutex;
    TaskState state = TaskState::Queued;
    string error;
    string result;
    bool cancel_requested = false;

    void set_state(TaskState s)
    {
        std::lock_guard lock(mutex);
        state = s;
        if (s == TaskState::Done || s == TaskState::Error || s == TaskState::Cancelled)
        {
            finished_at = std::chrono::steady_clock::now();
        }
    }

    [[nodiscard]] TaskState get_state() const
    {
        std::lock_guard lock(mutex);
        return state;
    }

    void request_cancel()
    {
        {
            std::lock_guard lock(mutex);
            cancel_requested = true;
            if (state == TaskState::Queued)
            {
                state = TaskState::Cancelled;
                finished_at = std::chrono::steady_clock::now();
            }
        }
        buffer->cancel();
    }

    [[nodiscard]] bool is_cancel_requested() const
    {
        std::lock_guard lock(mutex);
        return cancel_requested;
    }

    void set_result(string text)
    {
        std::lock_guard lock(mutex);
        result = std::move(text);
    }

    [[nodiscard]] string get_result() const
    {
        std::lock_guard lock(mutex);
        return result;
    }

    void set_error(std::string err)
    {
        std::lock_guard lock(mutex);
        error = std::move(err);
        state = TaskState::Error;
        finished_at = std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::string get_error() const
    {
        std::lock_guard lock(mutex);
        return error;
    }
};

/**
 * Single-flight GPU worker + short queue (max ~2 clients).
 * POST enqueues; worker runs LlamaEngine::chat; tokens go to TokenBuffer.
 */
class Jobs
{
  public:
    static constexpr size_t kMaxQueue = 16;
    static constexpr std::chrono::seconds kFinishedTtl{300};

    explicit Jobs(LlamaEngine &engine);

    ~Jobs();

    Jobs(const Jobs &) = delete;
    Jobs &operator=(const Jobs &) = delete;

    /**
     * Enqueue a chat job.
     * @param on_finished optional callback invoked once when task finishes (any terminal state).
     * @return key, or nullopt if queue is full (caller should 503).
     */
    optional<Task::Key> submit(MessagesRequest request,
                               std::function<void(const Task &)> on_finished = nullptr);

    shared_ptr<Task> get_task(const Task::Key &key);

    /** Cancel and mark for GC. */
    bool cancel(const Task::Key &key);

    void gc();

    void stop();

  private:
    /**
     * Garbage collect unsafely. It is assumed that you've locked the mutex beforehand
     */
    void unsafe_gc();

    void worker_loop();

    /**
     * @return A new, unique, key
     */
    Task::Key next_key();

    /**
     * @return Pop the queue for new tasks unsafely. It is assumed that you've locked the mutex beforehand
     */
    std::shared_ptr<Task> unsafe_pop_next_queued();

    LlamaEngine &engine_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::shared_ptr<Task>> tasks_;
    std::deque<std::string> queue_;
    std::atomic<uint64_t> key_counter_{1};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
