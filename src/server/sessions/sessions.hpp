#pragma once

#include "api/errors.hpp"
#include "server/sessions/tool_history.hpp"
#include "server/agent/response_parse.hpp"
#include "server/api/messages.hpp"
#include "server/api/sessions.hpp"
#include "server/jobs/jobs.hpp"
#include "server/sessions/session_disk.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Active session. Only one generation at a time; turns go through Jobs.
 *
 * Thread-safety: all public methods take mutex_. Callers must not lock it.
 */
class Session
{
  public:
    explicit Session(const ModelAdapter &adapter);
    Session(const ModelAdapter &adapter, SessionID id);

    // Conversation as stored in <id>.json. A restarted server has no running job.
    [[nodiscard]] SessionRecord record() const;
    void restore(SessionRecord record);

    // Unique ID for this session
    const SessionID id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // Timestamp when the session was created
    const std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();

    void touch();

    // Wall clock stored in <id>.json. A resume or a finished turn moves it to now.
    void mark_updated();

    [[nodiscard]] int64_t updated_at() const;

    [[nodiscard]] std::chrono::steady_clock::time_point last_active() const;

    [[nodiscard]] std::optional<JobKey> active_job() const;

    [[nodiscard]] std::shared_ptr<Task> latest_finished_job() const;

    /** True if this session tracks the job (active or last finished). */
    [[nodiscard]] bool owns_job(JobKey job_key) const;

    /**
     * Resolve a job that belongs to this session.
     * Prefers the live Jobs registry, then falls back to latest finished task.
     */
    [[nodiscard]] std::shared_ptr<Task> resolve_job(JobKey job_key, const Jobs &jobs) const;

    // with_messages false is the session GET: state and pending work, not the transcript.
    [[nodiscard]] SessionResponse to_response(bool with_messages = true) const;

    void set_context_usage(int used, int size);

    [[nodiscard]] std::pair<int, int> context_usage() const;

    /**
     * Idle long enough to collect. A session waiting on the client is kept:
     * the tool-result or answer POST still has to land on this id.
     */
    [[nodiscard]] bool is_gc_idle() const;

    // Copy of the conversation a sub-agent can continue from. The system prompt is kept as-is.
    struct Clone
    {
        std::string system_prompt;
        std::vector<ChatMessage> messages;
        std::vector<ChatTool> tools;
        bool questions = true;
        bool compress_tools = true;
        int max_tokens = -1;
    };

    [[nodiscard]] Clone capture(std::span<const std::string_view> omit_tools) const;

    void load_clone(Clone clone);

    void configure(CreateSessionRequest req);

    /**
     * Append a user message and clear pending waits.
     * @throws Busy if a generation is already in flight.
     */
    void accept_user_message(const SessionMessageRequest &msg);

    /**
     * Append tool results and clear pending tool wait.
     * @throws Busy / BadRequest on invalid state.
     */
    void accept_tool_results(const SessionToolResultsRequest &body);

    /**
     * Claim a new generation: set active_job, Generating, snapshot request onto task.
     * Call before Jobs::enqueue so ownership is registered first.
     * @throws Busy if already generating.
     */
    std::shared_ptr<Task> begin_generation();

    /**
     * Apply a finished job (any terminal state). Ignores stale keys.
     * Takes a status snapshot via Task::to_status (locks task mutex while session held).
     */
    void complete_job(std::shared_ptr<Task> task);

    friend std::ostream &operator<<(std::ostream &o, const Session &s)
    {
        return o << "Session(" << s.id << ")";
    }

    friend std::ostream &operator<<(std::ostream &o, const std::shared_ptr<Session> &s)
    {
        if (!s)
        {
            return o << "Session(null)";
        }
        return o << *s;
    }

  private:
    mutable std::mutex mutex_;
    std::string system_prompt_;
    std::vector<ChatMessage> messages_;
    std::vector<ChatTool> tools_;
    int max_tokens_ = -1;
    int turn_max_tokens_ = -1;
    int context_used_ = 0;
    int context_size_ = 0;
    std::optional<JobKey> active_job_;
    std::shared_ptr<Task> latest_finished_;
    SessionState state_ = SessionState::Idle;
    std::string last_error_;
    std::string error_code_;
    // Captured at the start of the turn that was just appended, so a context-full
    // failure can remove that message and put the session back.
    bool rollback_armed_ = false;
    SessionState rollback_state_ = SessionState::Idle;
    std::vector<ParsedToolCall> rollback_tools_;
    std::optional<ParsedQuestion> rollback_question_;
    const ModelAdapter &adapter_;
    std::vector<ParsedToolCall> pending_tool_calls_;
    std::optional<ParsedQuestion> pending_question_;
    bool questions_enabled_ = true;
    bool compress_tools_ = true;
    std::chrono::steady_clock::time_point last_active_ = std::chrono::steady_clock::now();
    int64_t updated_at_ = 0;

    void mark_updated_unlocked();

    [[nodiscard]] bool tracks_job_unlocked(JobKey job_key) const;

    void clear_pending_unlocked();

    // Tool bodies already in the session become one line. The caller appends the message that stays intact.
    void shrink_stored_tool_results_unlocked();

    void ensure_no_active_generation_unlocked() const;

    static std::string format_tool_results(const SessionToolResultsRequest &body, bool annotate);

    void arm_rollback_unlocked();

    void disarm_rollback_unlocked();

    void rollback_last_turn_unlocked();

    ParsedAssistantActions actions_from_status(const MessageStatusResponse &status) const;

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

    explicit Sessions(Jobs &jobs, const ModelAdapter &adapter);
    ~Sessions();

    // Load <id>.json from this directory. Later creates and turns are written there.
    // Empty leaves sessions in memory only. cache_bytes 0 does not limit the directory.
    void load(const std::string &dir, uint64_t cache_bytes = 0);

    // The transcript was opened. Refresh updated_at and drop older sessions that no longer fit.
    void note_resume(SessionID id);

    // Drop the oldest session files until the directory is within the configured size.
    // keep is never removed by this call.
    void enforce_cache_limit(SessionID keep);

    [[nodiscard]] std::shared_ptr<Session> create(CreateSessionRequest req);

    /**
     * Child session with this session's messages, prompt, and tools.
     * sub_agent is omitted so the child cannot snapshot again.
     * KV is cloned on the worker so the child can reuse the parent's prefix.
     */
    [[nodiscard]] std::shared_ptr<Session> snapshot(const SessionID &id);
    [[nodiscard]] std::shared_ptr<Session> get(const SessionID &id);

    // Newest session first. Does not collect idle sessions.
    [[nodiscard]] std::vector<std::shared_ptr<Session>> list();
    [[nodiscard]] std::shared_ptr<Session> destroy(const SessionID &id);

    /**
     * Append a user turn and enqueue a generation job.
     * @return job key
     */
    [[nodiscard]] std::optional<JobKey> post_message(const SessionID &id, const SessionMessageRequest &msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     * @return new job key
     */
    [[nodiscard]] std::optional<JobKey> post_tool_results(const SessionID &id, const SessionToolResultsRequest &body);

    void gc();

  private:
    Jobs &jobs_;
    const ModelAdapter &adapter_;
    std::mutex mutex_;
    std::string dir_;
    uint64_t cache_limit_ = 0;
    std::unordered_map<SessionID, std::shared_ptr<Session>> sessions_;

    void save_session(const std::shared_ptr<Session> &session);

    void unsafe_gc();

    std::optional<JobKey> enqueue_generation(const std::shared_ptr<Session> &session);

    void on_job_finished(const std::shared_ptr<Session> &session, std::shared_ptr<Task> task);
};
