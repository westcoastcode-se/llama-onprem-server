#pragma once

#include "../../common/std.hpp"
#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "../jobs/token_buffer.hpp"
#include "../llm/llm_engine.hpp"

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

enum class TaskState
{
    Queued,
    Running,
    Done,
    Error,
    Cancelled
};

inline const char *to_string(TaskState s)
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
    using Key = std::string;

    Key key;
    MessagesRequest request;
    std::shared_ptr<TokenBuffer> buffer = std::make_shared<TokenBuffer>();

    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished_at{};

    // Optional hook when task reaches a terminal state (done/error/cancelled).
    std::function<void(const Task &)> on_finished;

    mutable std::mutex mutex;
    TaskState state = TaskState::Queued;
    std::string error;
    std::string result;
    std::atomic<bool> cancel_requested{false};

    // Parsed after successful generation (client-side tools / questions).
    std::vector<ParsedToolCall> tool_calls;
    std::optional<ParsedQuestion> question;

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

    void set_error(std::string msg)
    {
        std::lock_guard lock(mutex);
        error = std::move(msg);
        state = TaskState::Error;
        finished_at = std::chrono::steady_clock::now();
    }

    void set_result(std::string text)
    {
        std::lock_guard lock(mutex);
        result = std::move(text);
    }

    [[nodiscard]] std::string get_result() const
    {
        std::lock_guard lock(mutex);
        return result;
    }

    [[nodiscard]] std::string get_error() const
    {
        std::lock_guard lock(mutex);
        return error;
    }

    void request_cancel()
    {
        cancel_requested.store(true, std::memory_order_relaxed);
        if (buffer)
        {
            buffer->cancel();
        }
    }

    [[nodiscard]] bool is_cancel_requested() const
    {
        return cancel_requested.load(std::memory_order_relaxed);
    }

    void set_actions(ParsedAssistantActions actions)
    {
        std::lock_guard lock(mutex);
        tool_calls = std::move(actions.tool_calls);
        question = std::move(actions.question);
    }

    [[nodiscard]] MessageStatusResponse to_status() const
    {
        std::lock_guard lock(mutex);
        MessageStatusResponse r;
        r.key = key;
        r.state = to_string(state);
        r.done = state == TaskState::Done || state == TaskState::Error || state == TaskState::Cancelled;
        r.content = result;
        r.error = error;
        r.tool_calls = tool_calls;
        r.question = question;
        return r;
    }
};

/**
 * Single-flight GPU worker + short queue (max 2).
 * REST clients get a key immediately; tokens stream via TokenBuffer.
 */
class Jobs
{
  public:
    static constexpr size_t kMaxQueue = 2;
    static constexpr std::chrono::seconds kFinishedTtl{300};

    explicit Jobs(LlamaEngine &engine);
    ~Jobs();

    Jobs(const Jobs &) = delete;
    Jobs &operator=(const Jobs &) = delete;

    void stop();

    /**
     * Enqueue a chat job.
     * @param on_finished optional callback invoked once when task finishes (any terminal state).
     * @return key, or nullopt if queue is full (caller should 503).
     */
    std::optional<Task::Key> submit(MessagesRequest request,
                                    std::function<void(const Task &)> on_finished = nullptr);

    std::shared_ptr<Task> get_task(const Task::Key &key);

    /**
     *
     * @param key The task key
     * @return
     */
    bool cancel(const Task::Key &key);

    void gc();

  private:
    LlamaEngine &engine_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<Task::Key, std::shared_ptr<Task>> tasks_;
    std::deque<Task::Key> queue_;
    std::atomic<uint64_t> key_counter_{1};
    std::atomic<bool> stop_{false};
    std::thread worker_;

    Task::Key next_key();
    void worker_loop();
    void unsafe_gc();
    std::shared_ptr<Task> unsafe_pop_next_queued();
};
