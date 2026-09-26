#pragma once

#include "../../common/std.hpp"
#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "../api/sessions.hpp"
#include "../jobs/jobs.hpp"
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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
    SessionID id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // Timestamp when the session was created
    std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();

    void touch()
    {
        std::lock_guard lock(mutex_);
        last_active_ = std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point last_active() const
    {
        std::lock_guard lock(mutex_);
        return last_active_;
    }

    [[nodiscard]] optional<JobKey> active_job() const
    {
        std::lock_guard lock(mutex_);
        return active_job_;
    }

    [[nodiscard]] shared_ptr<Task> latest_finished_job() const
    {
        std::lock_guard lock(mutex_);
        return latest_finished_;
    }

    /** True if this session tracks the job (active or last finished). */
    [[nodiscard]] bool owns_job(JobKey job_key) const
    {
        std::lock_guard lock(mutex_);
        return tracks_job_unlocked(job_key);
    }

    /**
     * Resolve a job that belongs to this session.
     * Prefers the live Jobs registry, then falls back to latest finished task.
     */
    [[nodiscard]] shared_ptr<Task> resolve_job(JobKey job_key, const Jobs &jobs) const
    {
        std::lock_guard lock(mutex_);
        if (!tracks_job_unlocked(job_key))
        {
            return {};
        }
        if (auto live = jobs.get_task(job_key))
        {
            return live;
        }
        if (latest_finished_ && latest_finished_->key == job_key)
        {
            return latest_finished_;
        }
        return {};
    }

    [[nodiscard]] SessionResponse to_response() const
    {
        std::lock_guard lock(mutex_);
        SessionResponse r;
        r.id = id;
        r.system_prompt = system_prompt_;
        r.messages = messages_;
        r.active_job_key = active_job_;
        r.state = active_job_ ? SessionState::Generating : state_;
        r.pending_tool_calls = pending_tool_calls_;
        r.pending_question = pending_question_;
        r.questions = questions_enabled_;
        return r;
    }

    /** Idle and no active job — candidate for GC. */
    [[nodiscard]] bool is_gc_idle() const
    {
        std::lock_guard lock(mutex_);
        return !active_job_ && state_ == SessionState::Idle;
    }

    void configure(CreateSessionRequest req)
    {
        std::lock_guard lock(mutex_);
        questions_enabled_ = req.questions;
        if (req.system.empty())
        {
            system_prompt_ = default_agent_system_prompt("", req.questions);
        }
        else
        {
            system_prompt_ = std::move(req.system);
        }
        messages_ = std::move(req.messages);
        last_active_ = std::chrono::steady_clock::now();
    }

    /**
     * Append a user message and clear pending waits.
     * @throws Busy if a generation is already in flight.
     */
    void accept_user_message(const SessionMessageRequest &msg)
    {
        std::lock_guard lock(mutex_);
        ensure_no_active_generation_unlocked();
        clear_pending_unlocked();
        state_ = SessionState::Idle;
        messages_.push_back(ChatMessage{.role = msg.role, .content = msg.content});
        last_active_ = std::chrono::steady_clock::now();
    }

    /**
     * Append tool results and clear pending tool wait.
     * @throws Busy / BadRequest on invalid state.
     */
    void accept_tool_results(const SessionToolResultsRequest &body)
    {
        std::lock_guard lock(mutex_);
        ensure_no_active_generation_unlocked();
        if (state_ != SessionState::AwaitingTools && pending_tool_calls_.empty())
        {
            throw BadRequest("session is not awaiting tool results");
        }

        messages_.push_back(
            ChatMessage{.role = ChatMessage::ROLE_USER, .content = format_tool_results(body)});
        clear_pending_unlocked();
        state_ = SessionState::Idle;
        last_active_ = std::chrono::steady_clock::now();
    }

    /**
     * Claim a new generation: set active_job, Generating, snapshot request onto task.
     * Call before Jobs::enqueue so ownership is registered first.
     * @throws Busy if already generating.
     */
    shared_ptr<Task> begin_generation()
    {
        auto task = std::make_shared<Task>();
        std::lock_guard lock(mutex_);
        ensure_no_active_generation_unlocked();
        task->request.system = system_prompt_;
        task->request.messages = messages_;
        state_ = SessionState::Generating;
        clear_pending_unlocked();
        last_active_ = std::chrono::steady_clock::now();
        active_job_ = task->key;
        latest_finished_.reset();
        return task;
    }

    /**
     * Apply a finished job (any terminal state). Ignores stale keys.
     * Takes a status snapshot via Task::to_status (locks task mutex while session held).
     */
    void complete_job(shared_ptr<Task> task)
    {
        std::lock_guard lock(mutex_);
        if (!active_job_ || *active_job_ != task->key)
        {
            log_error("Session(", id, ") | was notified by stale Task(", task->key, ")");
            return;
        }

        const auto status = task->to_status();
        active_job_.reset();
        last_active_ = std::chrono::steady_clock::now();
        latest_finished_ = std::move(task);

        if (status.state != JobState::Done)
        {
            state_ = SessionState::Idle;
            clear_pending_unlocked();
            return;
        }

        messages_.push_back(
            ChatMessage{.role = ChatMessage::ROLE_ASSISTANT, .content = status.content});

        auto actions = actions_from_status(status);
        if (!questions_enabled_)
        {
            actions.question.reset();
        }

        clear_pending_unlocked();
        if (!actions.tool_calls.empty())
        {
            pending_tool_calls_ = std::move(actions.tool_calls);
            pending_question_ = std::move(actions.question);
            state_ = SessionState::AwaitingTools;
        }
        else if (actions.question)
        {
            pending_question_ = std::move(actions.question);
            state_ = SessionState::AwaitingQuestion;
        }
        else
        {
            state_ = SessionState::Idle;
        }
    }

    friend std::ostream &operator<<(std::ostream &o, const Session &s)
    {
        return o << "Session(" << s.id << ")";
    }

    friend std::ostream &operator<<(std::ostream &o, const shared_ptr<Session> &s)
    {
        if (!s)
        {
            return o << "Session(null)";
        }
        return o << *s;
    }

  private:
    mutable std::mutex mutex_;
    string system_prompt_;
    vector<ChatMessage> messages_;
    optional<JobKey> active_job_;
    shared_ptr<Task> latest_finished_;
    SessionState state_ = SessionState::Idle;
    vector<ParsedToolCall> pending_tool_calls_;
    optional<ParsedQuestion> pending_question_;
    bool questions_enabled_ = true;
    std::chrono::steady_clock::time_point last_active_ = std::chrono::steady_clock::now();

    [[nodiscard]] bool tracks_job_unlocked(JobKey job_key) const
    {
        if (active_job_ && *active_job_ == job_key)
        {
            return true;
        }
        return latest_finished_ && latest_finished_->key == job_key;
    }

    void clear_pending_unlocked()
    {
        pending_tool_calls_.clear();
        pending_question_.reset();
    }

    void ensure_no_active_generation_unlocked() const
    {
        if (active_job_ || state_ == SessionState::Generating)
        {
            throw Busy("session already has an active generation");
        }
    }

    static string format_tool_results(const SessionToolResultsRequest &body)
    {
        std::ostringstream combined;
        for (size_t i = 0; i < body.results.size(); ++i)
        {
            const auto &r = body.results[i];
            if (i > 0)
            {
                combined << "\n";
            }
            if (r.denied)
            {
                combined << "<tool_response>\nerror: tool execution was denied by the user";
                if (!r.name.empty())
                {
                    combined << " for tool '" << r.name << "'";
                }
                else if (!r.id.empty())
                {
                    combined << " for tool id '" << r.id << "'";
                }
                combined << ".\n</tool_response>";
            }
            else
            {
                combined << "<tool_response>\n" << r.content << "\n</tool_response>";
            }
        }
        return combined.str();
    }

    static ParsedAssistantActions actions_from_status(const MessageStatusResponse &status)
    {
        ParsedAssistantActions actions;
        actions.tool_calls = status.tool_calls;
        actions.question = status.question;
        if (actions.tool_calls.empty() && !actions.question && !status.content.empty())
        {
            actions = parse_assistant_actions(status.content);
        }
        return actions;
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

    shared_ptr<Session> create(CreateSessionRequest req);
    shared_ptr<Session> get(const SessionID &id);
    shared_ptr<Session> destroy(const SessionID &id);

    /**
     * Append a user turn and enqueue a generation job.
     * @return job key
     */
    optional<JobKey> post_message(const SessionID &id, const SessionMessageRequest &msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     * @return new job key
     */
    optional<JobKey> post_tool_results(const SessionID &id, const SessionToolResultsRequest &body);

    void gc();

  private:
    Jobs &jobs_;
    std::mutex mutex_;
    std::unordered_map<SessionID, shared_ptr<Session>> sessions_;

    void unsafe_gc();

    optional<JobKey> enqueue_generation(const shared_ptr<Session> &session);

    void on_job_finished(const shared_ptr<Session> &session, shared_ptr<Task> task);
};
