#pragma once

#include "../std.hpp"
#include "agent/response_parse.hpp"
#include "api/messages.hpp"
#include "api/sessions.hpp"
#include "jobs/jobs.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class SessionWaitState
{
    // Waiting for client messages
    Idle,
    // Generating a response
    Generating,
    // Waiting for a tool responses
    AwaitingTools,
    // Waiting for answers of questions sent to the client
    AwaitingQuestion
};

inline const char *to_string(const SessionWaitState s)
{
    switch (s)
    {
    case SessionWaitState::Idle:
        return "idle";
    case SessionWaitState::Generating:
        return "generating";
    case SessionWaitState::AwaitingTools:
        return "awaiting_tools";
    case SessionWaitState::AwaitingQuestion:
        return "awaiting_question";
    }
    return "unknown";
}

struct Session
{
    using Key = std::string;

    Key id;
    string system;
    vector<ChatMessage> messages;
    Task::Key active_job_key;
    SessionWaitState wait_state = SessionWaitState::Idle;
    vector<ParsedToolCall> pending_tool_calls;
    optional<ParsedQuestion> pending_question;
    /** When false, do not pause on model <question> tags or teach question protocol. */
    bool questions_enabled = true;
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
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
     * @return Response object based on the session
     */
    [[nodiscard]] SessionResponse to_response() const
    {
        std::lock_guard lock(mutex);

        // caller must hold mutex or own exclusive access
        SessionResponse r;
        r.id = id;
        r.system = system;
        r.messages = messages;
        r.active_job_key = active_job_key;
        r.state = to_string(wait_state);
        // If generating, prefer that over stale wait flags
        if (!active_job_key.empty())
        {
            r.state = "generating";
        }
        r.pending_tool_calls = pending_tool_calls;
        r.pending_question = pending_question;
        r.questions = questions_enabled;
        return r;
    }
};

/**
 * Server-side chat sessions: durable message history, turns go through Jobs.
 * Tool calls and questions are returned to the client; the server does not execute tools.
 */
class Sessions
{
  public:
    static constexpr size_t kMaxSessions = 32;
    static constexpr std::chrono::seconds kIdleTtl{3600};

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
    shared_ptr<Session> get(const Session::Key &id);

    /**
     * Destroy the session with the supplied key
     *
     * @param id The session id
     * @return The destroyed session if found; empty otherwise
     */
    shared_ptr<Session> destroy(const Session::Key &id);

    /**
     * Append a user turn and enqueue a generation job.
     *
     * @return job key, or nullopt if queue full.
     */
    optional<Task::Key> post_message(const Session::Key &id, SessionMessageRequest msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     *
     * @return new job key, or nullopt if queue full.
     */
    optional<Task::Key> post_tool_results(const Session::Key &id, SessionToolResultsRequest body);

    void gc();

  private:
    Jobs &jobs_;
    std::mutex mutex_;
    std::unordered_map<Session::Key, std::shared_ptr<Session>> sessions_;
    std::atomic<uint64_t> id_counter_{1};

    Session::Key next_id();

    void unsafe_gc();

    optional<Task::Key> enqueue_generation(const shared_ptr<Session> &session);

    void on_job_finished(const std::shared_ptr<Session> &session, const Task &task);
};
