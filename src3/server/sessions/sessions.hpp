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
    optional<Task::Key> active_job_key;
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
        r.active_job_key = active_job_key;
        r.state = state;
        // If generating, prefer that over stale wait flags
        if (active_job_key)
        {
            r.state = SessionState::Generating;
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
    optional<Task::Key> post_message(const SessionID &id, const SessionMessageRequest& msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     *
     * @return new job key, or empty if queue full.
     */
    optional<Task::Key> post_tool_results(const SessionID &id, const SessionToolResultsRequest& body);

    void gc();

  private:
    Jobs &jobs_;
    std::mutex mutex_;
    std::unordered_map<SessionID, shared_ptr<Session>> sessions_;

    void unsafe_gc();

    optional<Task::Key> enqueue_generation(const shared_ptr<Session> &session);

    /**
     * Method called when a job is finished
     *
     * @param session The session
     * @param task The task
     */
    void on_job_finished(const std::shared_ptr<Session> &session, const Task &task);
};
