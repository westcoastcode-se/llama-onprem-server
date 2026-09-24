#pragma once

#include "../../common/std.hpp"
#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "../jobs/token_buffer.hpp"
#include "../llm/llm_engine.hpp"
#include "common/log.hpp"

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

struct Task
{
    // Unique ID for the task
    JobKey key = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    MessagesRequest request;

    // Buffer used to send data between the job runner and the eventual task stream
    shared_ptr<TokenBuffer> buffer = std::make_shared<TokenBuffer>();

    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished_at{};

    // Lambda called when reaching an exit state
    std::function<void(const shared_ptr<Task>&)> on_finished;

    mutable std::mutex mutex;
    JobState state = JobState::Queued;
    string error;
    string result;
    std::atomic<bool> cancel_requested{false};

    // Parsed after successful generation (client-side tools / questions).
    std::vector<ParsedToolCall> tool_calls;
    std::optional<ParsedQuestion> question;

    [[nodiscard]] JobState get_state() const
    {
        std::lock_guard lock(mutex);
        return state;
    }

    /**
     * Set the error message
     *
     * @param msg The error message
     */
    void set_error_state(string msg)
    {
        std::lock_guard lock(mutex);
        error = std::move(msg);
        state = JobState::Error;
        finished_at = std::chrono::steady_clock::now();
        buffer->set_done();
    }

    /**
     * Set the result from the LLM
     *
     * @param text The resulting text
     * @param new_state The state of the job
     */
    void set_result(string text, const JobState new_state, ParsedAssistantActions actions)
    {
        std::lock_guard lock(mutex);
        result = std::move(text);
        state = new_state;
        tool_calls = std::move(actions.tool_calls);
        question = std::move(actions.question);

        // Job is finished
        if (new_state.is_finished())
        {
            finished_at = std::chrono::steady_clock::now();
        }

        if (new_state == JobState::Cancelled)
        {
            buffer->cancel();
        }
        else if (new_state == JobState::Done)
        {
            buffer->set_done();
        }
    }

    [[nodiscard]] string get_result() const
    {
        std::lock_guard lock(mutex);
        return result;
    }

    [[nodiscard]] string get_error() const
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

    [[nodiscard]] MessageStatusResponse to_status_unsafe() const
    {
        MessageStatusResponse r;
        r.key = key;
        r.state = state;
        r.done = state.is_finished();
        r.content = result;
        r.error = error;
        r.tool_calls = tool_calls;
        r.question = question;
        return r;
    }

    [[nodiscard]] MessageStatusResponse to_status() const
    {
        std::lock_guard lock(mutex);
        MessageStatusResponse r;
        r.key = key;
        r.state = state;
        r.done = state.is_finished();
        r.content = result;
        r.error = error;
        r.tool_calls = tool_calls;
        r.question = question;
        return r;
    }

    /**
     * Set this task as cancelled
     */
    void set_cancelled()
    {
        std::lock_guard lock(mutex);
        state = JobState::Cancelled;
        buffer->cancel();
    }

    void set_running()
    {
        std::lock_guard lock(mutex);
        state = JobState::Running;
    }

    friend std::ostream &operator<<(std::ostream &o, const Task &ptr)
    {
        return o << "Task(" << ptr.key << ")";
    }
};

/**
 * Single-flight GPU worker
 *
 * REST clients get a key immediately; tokens stream via TokenBuffer.
 */
class Jobs
{
  public:
    // TODO: Consider adding support for forcefully stopping long-running LLM requests
    // TODO: Wait to GC until first stream request is called?
    static constexpr std::chrono::seconds kFinishedTtl{300};

    explicit Jobs(LlamaEngine &engine);
    ~Jobs();

    Jobs(const Jobs &) = delete;
    Jobs &operator=(const Jobs &) = delete;

    void stop();

    /**
     * Enqueue a chat job.
     *
     * @param request The message to be processed by the LLM. This method takes over ownership of it
     * @param on_finished optional callback invoked once when task finishes (any terminal state).
     * @return A unique key that represents the job
     */
    JobKey submit(MessagesRequest &&request, std::function<void(const shared_ptr<Task>&)> on_finished = nullptr);

    /**
     * @param key The task id
     * @return The task if found
     */
    shared_ptr<Task> get_task(JobKey key) const;

    /**
     * Try to cancel the supplied task
     *
     * @param id The task
     * @return The cancelled task. Please note that the task might not be cancelled yet, but only requested
     *         to be cancelled
     */
    shared_ptr<Task> cancel(JobKey id);

    void gc();

  private:
    LlamaEngine &engine_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<JobKey, std::shared_ptr<Task>> tasks_;
    std::deque<JobKey> queue_;
    std::atomic<bool> stop_{false};
    std::thread worker_;

    void worker_loop();
    void unsafe_gc();

    /**
     * @return The next queued task. The task might be cancelled
     */
    shared_ptr<Task> pop_next_queued();
};
