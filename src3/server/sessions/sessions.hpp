#pragma once

#include "../../common/std.hpp"
#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "../api/sessions.hpp"
#include "../jobs/jobs.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Active session. Only one session can interact with the LLM at the same time.
 *
 * If a chat requests get in between this session and another then the context will
 * be reset and a new context will be created and prepared based on the chat history
 * of the new session.
 */
struct Session
{
    // Unique ID for this session
    SessionID id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // The system prompt
    string system_prompt;
    // All chat messages associated with this session
    vector<ChatMessage> messages;
    // Job currently owned by this session (queued or running)
    optional<JobKey> active_task;
    // Last completed job for this session (status/token tail after active_task is cleared)
    shared_ptr<Task> latest_finished_task;
    // What state the session is in
    SessionState state = SessionState::Idle;
    vector<ParsedToolCall> pending_tool_calls;
    optional<ParsedQuestion> pending_question;
    /** When false, do not pause on model <question> tags or teach question protocol. */
    bool questions_enabled = true;
    // Timestamp when the session was created
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
    // Timestamp when the session was last active. Normally used by the server
    // for garbage collection
    std::chrono::steady_clock::time_point last_active = std::chrono::steady_clock::now();

    mutable std::mutex mutex;

    void touch()
    {
        last_active = std::chrono::steady_clock::now();
    }

    void clear_pending()
    {
        pending_tool_calls.clear();
        pending_question.reset();
    }

    /**
     * @return The current active job key, if any
     */
    optional<JobKey> get_active_task() const
    {
        std::lock_guard slock(mutex);
        return active_task;
    }

    shared_ptr<Task> get_latest_finished_task() const
    {
        std::lock_guard slock(mutex);
        return latest_finished_task;
    }

    /**
     * True if this session currently tracks the job (active or last finished).
     */
    [[nodiscard]] bool owns_job(JobKey job_key) const
    {
        std::lock_guard slock(mutex);
        if (active_task && *active_task == job_key)
        {
            return true;
        }
        return latest_finished_task && latest_finished_task->key == job_key;
    }

    /**
     * Resolve a job that belongs to this session.
     * Prefers the live Jobs registry, then falls back to latest_finished_task.
     */
    [[nodiscard]] shared_ptr<Task> resolve_job(JobKey job_key, const Jobs &jobs) const
    {
        std::lock_guard slock(mutex);
        const bool is_active = active_task && *active_task == job_key;
        const bool is_latest = latest_finished_task && latest_finished_task->key == job_key;
        if (!is_active && !is_latest)
        {
            return {};
        }

        if (auto live = jobs.get_task(job_key))
        {
            return live;
        }
        if (is_latest)
        {
            return latest_finished_task;
        }
        return {};
    }

    /**
     * @return Response object based on the session
     */
    [[nodiscard]] SessionResponse to_response() const
    {
        std::lock_guard lock(mutex);

        // caller must hold mutex or own exclusive access
        SessionResponse r;
        r.id = id;
        r.system_prompt = system_prompt;
        r.messages = messages;
        r.active_job_key = active_task;
        r.state = state;
        // If generating, prefer that over stale wait flags
        if (active_task)
        {
            r.state = SessionState::Generating;
        }
        r.pending_tool_calls = pending_tool_calls;
        r.pending_question = pending_question;
        r.questions = questions_enabled;
        return r;
    }

    friend std::ostream &operator<<(std::ostream &o, const Session &ptr)
    {
        return o << "Session(" << ptr.id << ")";
    }
};

/**
 * Server-side chat sessions: durable message history, turns go through Jobs.
 * Tool calls and questions are returned to the client; the server does not execute tools.
 */
class Sessions
{
  public:
    static constexpr size_t kMaxSessions = 256;
    static constexpr std::chrono::seconds kIdleTtl{600};

    explicit Sessions(Jobs &jobs);

    /**
     * Create a new session
     *
     * @param req The session request
     * @return A new session
     */
    shared_ptr<Session> create(CreateSessionRequest req);

    /**
     * Get a session using the supplied id
     *
     * @param id The session key
     * @return A session if found; empty otherwise
     */
    shared_ptr<Session> get(const SessionID &id);

    /**
     * Destroy the session with the supplied key
     *
     * @param id The session id
     * @return The destroyed session if found; empty otherwise
     */
    shared_ptr<Session> destroy(const SessionID &id);

    /**
     * Append a user turn and enqueue a generation job.
     *
     * @return job key, or nullopt if queue full.
     */
    optional<JobKey> post_message(const SessionID &id, const SessionMessageRequest &msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     *
     * @return new job key, or empty if queue full.
     */
    optional<JobKey> post_tool_results(const SessionID &id, const SessionToolResultsRequest &body);

    void gc();

  private:
    Jobs &jobs_;
    std::mutex mutex_;
    std::unordered_map<SessionID, shared_ptr<Session>> sessions_;

    void unsafe_gc();

    /**
     * Enqueue a new LLM text generation request to be processed as soon as a slot is available
     *
     * @param session The session
     * @return A job based on the currently running task for this session
     */
    optional<JobKey> enqueue_generation(const shared_ptr<Session> &session);

    /**
     * Method called when a job is finished. The job itself might've been cancelled
     *
     * @param session The session
     * @param task The task
     */
    void on_job_finished(const shared_ptr<Session> &session, shared_ptr<Task> task);
};
