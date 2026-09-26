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
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

inline JobKey next_job_key()
{
    static std::atomic<JobKey> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

struct Task
{
    // Unique ID for the task
    JobKey key = next_job_key();

    MessagesRequest request;

    // Buffer used to send data between the job runner and the eventual task stream
    shared_ptr<TokenBuffer> buffer = std::make_shared<TokenBuffer>();

    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished_at{};

    // Invoked once when the task reaches a terminal state.
    std::move_only_function<void(const shared_ptr<Task> &)> on_finished;

    /**
     * Move out on_finished under lock so the callback can run without holding mutex.
     */
    [[nodiscard]] std::move_only_function<void(const shared_ptr<Task> &)> take_on_finished()
    {
        std::lock_guard lock(mutex);
        return std::exchange(on_finished, nullptr);
    }

    mutable std::mutex mutex;
    JobState state = JobState::Queued;
    string error;
    string error_code;
    string result;
    string reasoning;
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
    void set_error_state(string msg, string code = {})
    {
        std::lock_guard lock(mutex);
        error = std::move(msg);
        error_code = std::move(code);
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
    void set_result(string text, const JobState new_state, ParsedAssistantActions actions = {})
    {
        std::lock_guard lock(mutex);
        result = std::move(text);
        reasoning = std::move(actions.reasoning);
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
        else if (new_state.is_finished())
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
        r.error_code = error_code;
        r.reasoning = reasoning;
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
        r.error_code = error_code;
        r.reasoning = reasoning;
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
        finished_at = std::chrono::steady_clock::now();
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
class ModelAdapter;

class Jobs
{
  public:
    static constexpr std::chrono::seconds kFinishedTtl{300};

    explicit Jobs(LlamaEngine &engine, const ModelAdapter &adapter);
    ~Jobs();

    Jobs(const Jobs &) = delete;
    Jobs &operator=(const Jobs &) = delete;

    void stop();

    // Cancel queued and running tasks and wake token streams. Does not join the worker.
    void request_shutdown();

    /**
     * Enqueue a chat job.
     *
     * @param request The message to be processed by the LLM. This method takes over ownership of it
     * @param on_finished optional callback invoked once when task finishes (any terminal state).
     * @return The created task (also retained by the job runner until GC).
     */
    shared_ptr<Task> submit(MessagesRequest &&request,
                            std::move_only_function<void(const shared_ptr<Task> &)> on_finished = {});

    /**
     * Enqueue an already constructed task. Prefer this when the caller must register the job key
     * on a session *before* the worker can finish (avoids lost on_finished updates).
     *
     * @param task Task with request/on_finished already set. Ownership is shared with Jobs.
     * @return The same task pointer.
     */
    shared_ptr<Task> enqueue(shared_ptr<Task> task);

    /**
     * @param key The task id
     * @return The task if found
     */
    [[nodiscard]] shared_ptr<Task> get_task(JobKey key) const;

    /**
     * Try to cancel the supplied task
     *
     * @param id The task
     * @return The cancelled task. Please note that the task might not be cancelled yet, but only requested
     *         to be cancelled
     */
    shared_ptr<Task> cancel(JobKey id);

    void gc();

    /**
     * Forget KV parked for this session. Applied on the worker thread.
     */
    void release_session(const std::string &session_id);

    /**
     * Copy KV from one session id onto another. Applied on the worker thread.
     */
    void clone_session(const std::string &from, const std::string &to);

  private:
    LlamaEngine &engine_;
    const ModelAdapter &adapter_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<JobKey, std::shared_ptr<Task>> tasks_;
    std::deque<JobKey> queue_;
    std::vector<std::string> pending_session_releases_;
    std::vector<std::pair<std::string, std::string>> pending_session_clones_;
    std::shared_ptr<Task> current_task_;
    std::jthread worker_;

    void worker_loop(std::stop_token stop);
    void unsafe_gc();

    struct NextWork
    {
        std::vector<std::string> releases;
        std::vector<std::pair<std::string, std::string>> clones;
        shared_ptr<Task> task;
    };

    /**
     * @return The next queued task, plus session KV releases to apply first.
     *         The task might be cancelled. Empty when shutting down.
     */
    NextWork wait_next(const std::stop_token &stop);
};
