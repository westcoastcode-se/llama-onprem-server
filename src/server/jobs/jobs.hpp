#pragma once

#include "common/log.hpp"
#include "server/agent/response_parse.hpp"
#include "server/api/messages.hpp"
#include "server/jobs/token_buffer.hpp"
#include "server/llm/llm_engine.hpp"

#include <algorithm>
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
    std::shared_ptr<TokenBuffer> buffer = std::make_shared<TokenBuffer>();

    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point finished_at{};

    // Invoked once when the task reaches a terminal state.
    std::move_only_function<void(const std::shared_ptr<Task> &)> on_finished;

    /**
     * Move out on_finished under lock so the callback can run without holding mutex.
     */
    [[nodiscard]] std::move_only_function<void(const std::shared_ptr<Task> &)> take_on_finished();

    mutable std::mutex mutex;
    JobState state = JobState::Queued;
    std::string error;
    std::string error_code;
    std::string result;
    std::string reasoning;
    std::atomic<bool> cancel_requested{false};
    std::atomic<int> context_used_{0};
    std::atomic<int> context_size_{0};

    // Parsed after successful generation (client-side tools / questions).
    std::vector<ParsedToolCall> tool_calls;
    std::optional<ParsedQuestion> question;

    [[nodiscard]] JobState get_state() const;

    /**
     * Set the error message
     *
     * @param msg The error message
     */
    void set_error_state(std::string msg, std::string code = {});

    /**
     * Set the result from the LLM
     *
     * @param text The resulting text
     * @param new_state The state of the job
     */
    void set_result(std::string text, const JobState new_state, ParsedAssistantActions actions = {});

    void release_stream();

    [[nodiscard]] std::string get_result() const;

    [[nodiscard]] std::string get_error() const;

    void request_cancel();

    [[nodiscard]] bool is_cancel_requested() const;

    // Latest KV size for this job's session. Written on the worker, read by the token stream.
    void note_context(int used, int size);

    [[nodiscard]] int context_used() const;

    [[nodiscard]] int context_size() const;

    [[nodiscard]] MessageStatusResponse to_status_unsafe() const;

    [[nodiscard]] MessageStatusResponse to_status() const;

    /**
     * Set this task as cancelled
     */
    void set_cancelled();

    void set_running();

    friend std::ostream &operator<<(std::ostream &o, const Task &ptr)
    {
        return o << "Task(" << ptr.key << ")";
    }

    friend std::ostream &operator<<(std::ostream &o, const std::shared_ptr<Task> &ptr)
    {
        if (!ptr)
        {
            return o << "Task(null)";
        }
        return o << *ptr;
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
    std::shared_ptr<Task> submit(MessagesRequest &&request,
                            std::move_only_function<void(const std::shared_ptr<Task> &)> on_finished = {});

    /**
     * Enqueue an already constructed task. Prefer this when the caller must register the job key
     * on a session *before* the worker can finish (avoids lost on_finished updates).
     *
     * @param task Task with request/on_finished already set. Ownership is shared with Jobs.
     * @return The same task pointer.
     */
    std::shared_ptr<Task> enqueue(std::shared_ptr<Task> task);

    /**
     * @param key The task id
     * @return The task if found
     */
    [[nodiscard]] std::shared_ptr<Task> get_task(JobKey key) const;

    /**
     * Try to cancel the supplied task
     *
     * @param id The task
     * @return The cancelled task. Please note that the task might not be cancelled yet, but only requested
     *         to be cancelled
     */
    std::shared_ptr<Task> cancel(JobKey id);

    void gc();

    /**
     * Forget KV parked for this session. Applied on the worker thread.
     */
    void release_session(const std::string &session_id);

    // Configured context length. Immutable after the engine is loaded.
    [[nodiscard]] int context_size() const;

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
        std::shared_ptr<Task> task;
    };

    /**
     * @return The next queued task, plus session KV releases to apply first.
     *         The task might be cancelled. Empty when shutting down.
     */
    NextWork wait_next(const std::stop_token &stop);
};
