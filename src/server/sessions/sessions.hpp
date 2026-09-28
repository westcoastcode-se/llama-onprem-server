#pragma once

#include "../../common/std.hpp"
#include "../../common/tool_history.hpp"
#include "../../api/errors.hpp"
#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "../api/sessions.hpp"
#include "../jobs/jobs.hpp"
#include <algorithm>
#include <chrono>
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
    explicit Session(const ModelAdapter &adapter) : adapter_(adapter)
    {
    }

    // Unique ID for this session
    const SessionID id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // Timestamp when the session was created
    const std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();

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
        r.compress_tools = compress_tools_;
        r.error = last_error_;
        r.error_code = error_code_;
        r.context_used = context_used_;
        r.context_size = context_size_;
        return r;
    }

    void set_context_usage(int used, int size)
    {
        std::lock_guard lock(mutex_);
        context_used_ = std::max(0, used);
        if (size > 0)
        {
            context_size_ = size;
        }
    }

    [[nodiscard]] std::pair<int, int> context_usage() const
    {
        std::lock_guard lock(mutex_);
        return {context_used_, context_size_};
    }

    /**
     * Idle long enough to collect. A session waiting on the client is kept:
     * the tool-result or answer POST still has to land on this id.
     */
    [[nodiscard]] bool is_gc_idle() const
    {
        std::lock_guard lock(mutex_);
        if (active_job_ || state_ == SessionState::Generating)
        {
            return false;
        }
        return state_ != SessionState::AwaitingTools && state_ != SessionState::AwaitingQuestion;
    }

    // Copy of the conversation a sub-agent can continue from. The system prompt is kept as-is.
    struct Clone
    {
        string system_prompt;
        vector<ChatMessage> messages;
        vector<ChatTool> tools;
        bool questions = true;
        bool compress_tools = true;
        int max_tokens = -1;
    };

    [[nodiscard]] Clone capture(std::span<const std::string_view> omit_tools) const
    {
        std::lock_guard lock(mutex_);
        Clone clone;
        clone.system_prompt = system_prompt_;
        clone.messages = messages_;
        clone.tools.reserve(tools_.size());
        for (const ChatTool &tool : tools_)
        {
            if (std::ranges::find(omit_tools, std::string_view(tool.name)) == omit_tools.end())
            {
                clone.tools.push_back(tool);
            }
        }
        clone.questions = questions_enabled_;
        clone.compress_tools = compress_tools_;
        clone.max_tokens = max_tokens_;
        return clone;
    }

    void load_clone(Clone clone)
    {
        std::lock_guard lock(mutex_);
        system_prompt_ = std::move(clone.system_prompt);
        messages_ = std::move(clone.messages);
        tools_ = std::move(clone.tools);
        questions_enabled_ = clone.questions;
        compress_tools_ = clone.compress_tools;
        max_tokens_ = clone.max_tokens;
        turn_max_tokens_ = max_tokens_;
        last_active_ = std::chrono::steady_clock::now();
    }

    void configure(CreateSessionRequest req)
    {
        std::lock_guard lock(mutex_);
        questions_enabled_ = req.questions;
        compress_tools_ = req.compress_tools;
        tools_ = std::move(req.tools);
        system_prompt_ = default_agent_system_prompt(adapter_, tools_, req.system, req.questions, req.compress_tools);
        messages_ = std::move(req.messages);
        max_tokens_ = req.max_tokens;
        turn_max_tokens_ = max_tokens_;
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
        // A question is still this turn: the model needs the tool results to continue.
        // Once the turn is idle, those results have been used and can leave the window.
        const bool turn_finished = state_ == SessionState::Idle;
        arm_rollback_unlocked();
        clear_pending_unlocked();
        state_ = SessionState::Idle;
        if (turn_finished)
        {
            shrink_stored_tool_results_unlocked();
        }
        messages_.push_back(ChatMessage{.role = msg.role, .content = msg.content, .reasoning_content = {}});
        turn_max_tokens_ = msg.max_tokens >= 0 ? msg.max_tokens : max_tokens_;
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

        arm_rollback_unlocked();
        // The new result stays intact. Earlier bodies become one line before this prompt is built,
        // so a long review does not keep every file it already acted on.
        shrink_stored_tool_results_unlocked();
        messages_.push_back(ChatMessage{.role = string(ChatMessage::ROLE_USER),
                                        .content = format_tool_results(body, compress_tools_),
                                        .reasoning_content = {}});
        turn_max_tokens_ = max_tokens_;
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
        task->request.session_id = std::to_string(id);
        task->request.max_tokens = turn_max_tokens_;
        task->request.tools = tools_;
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
        context_used_ = std::max(0, task->context_used());
        if (const int size = task->context_size(); size > 0)
        {
            context_size_ = size;
        }
        active_job_.reset();
        last_active_ = std::chrono::steady_clock::now();
        latest_finished_ = std::move(task);

        if (status.state == JobState::Error)
        {
            last_error_ = status.error.empty() ? "generation failed" : status.error;
            error_code_ = status.error_code;
            if (status.error_code == kContextFull)
            {
                rollback_last_turn_unlocked();
            }
            else
            {
                state_ = SessionState::Idle;
                clear_pending_unlocked();
            }
            disarm_rollback_unlocked();
            return;
        }
        if (status.state != JobState::Done)
        {
            last_error_.clear();
            error_code_.clear();
            state_ = SessionState::Idle;
            clear_pending_unlocked();
            disarm_rollback_unlocked();
            return;
        }

        last_error_.clear();
        error_code_.clear();
        disarm_rollback_unlocked();
        ChatMessage assistant;
        assistant.role = string(ChatMessage::ROLE_ASSISTANT);
        assistant.content = status.content;
        assistant.reasoning_content = status.reasoning;
        if (!assistant.content.empty() || !assistant.reasoning_content.empty())
        {
            messages_.push_back(std::move(assistant));
        }

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
    vector<ChatTool> tools_;
    int max_tokens_ = -1;
    int turn_max_tokens_ = -1;
    int context_used_ = 0;
    int context_size_ = 0;
    optional<JobKey> active_job_;
    shared_ptr<Task> latest_finished_;
    SessionState state_ = SessionState::Idle;
    string last_error_;
    string error_code_;
    // Captured at the start of the turn that was just appended, so a context-full
    // failure can remove that message and put the session back.
    bool rollback_armed_ = false;
    SessionState rollback_state_ = SessionState::Idle;
    vector<ParsedToolCall> rollback_tools_;
    optional<ParsedQuestion> rollback_question_;
    const ModelAdapter &adapter_;
    vector<ParsedToolCall> pending_tool_calls_;
    optional<ParsedQuestion> pending_question_;
    bool questions_enabled_ = true;
    bool compress_tools_ = true;
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

    // Tool bodies already in the session become one line. The caller appends the message that stays intact.
    void shrink_stored_tool_results_unlocked()
    {
        if (!compress_tools_)
        {
            return;
        }
        for (ChatMessage &message : messages_)
        {
            message.content = shrink_tool_responses(message.content);
        }
    }

    void ensure_no_active_generation_unlocked() const
    {
        if (active_job_ || state_ == SessionState::Generating)
        {
            throw Busy("session already has an active generation");
        }
    }

    static string format_tool_results(const SessionToolResultsRequest &body, bool annotate)
    {
        string combined;
        for (const auto &r : body.results)
        {
            if (!combined.empty())
            {
                combined.push_back('\n');
            }
            const string open = annotate ? tool_response_open(r.name, r.detail) : "<tool_response>";
            if (r.denied)
            {
                if (!r.name.empty())
                {
                    combined += std::format("{}\nerror: tool execution was denied by the user for tool '{}'.\n</tool_response>",
                                            open, r.name);
                }
                else if (!r.id.empty())
                {
                    combined += std::format(
                        "{}\nerror: tool execution was denied by the user for tool id '{}'.\n</tool_response>", open, r.id);
                }
                else
                {
                    combined += open + "\nerror: tool execution was denied by the user.\n</tool_response>";
                }
            }
            else
            {
                combined += std::format("{}\n{}\n</tool_response>", open, r.content);
            }
        }
        return combined;
    }

    void arm_rollback_unlocked()
    {
        rollback_armed_ = true;
        rollback_state_ = state_;
        rollback_tools_ = pending_tool_calls_;
        rollback_question_ = pending_question_;
    }

    void disarm_rollback_unlocked()
    {
        rollback_armed_ = false;
        rollback_tools_.clear();
        rollback_question_.reset();
    }

    void rollback_last_turn_unlocked()
    {
        if (rollback_armed_ && !messages_.empty() && messages_.back().role == ChatMessage::ROLE_USER)
        {
            messages_.pop_back();
            pending_tool_calls_ = rollback_tools_;
            pending_question_ = rollback_question_;
            state_ = rollback_state_;
            return;
        }
        state_ = SessionState::Idle;
        clear_pending_unlocked();
    }

    ParsedAssistantActions actions_from_status(const MessageStatusResponse &status) const
    {
        ParsedAssistantActions actions;
        actions.tool_calls = status.tool_calls;
        actions.question = status.question;
        if (actions.tool_calls.empty() && !actions.question && !status.content.empty())
        {
            actions = parse_assistant_actions(status.content, adapter_);
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

    explicit Sessions(Jobs &jobs, const ModelAdapter &adapter);

    [[nodiscard]] shared_ptr<Session> create(CreateSessionRequest req);

    /**
     * Child session with this session's messages, prompt, and tools.
     * sub_agent is omitted so the child cannot snapshot again.
     * KV is cloned on the worker so the child can reuse the parent's prefix.
     */
    [[nodiscard]] shared_ptr<Session> snapshot(const SessionID &id);
    [[nodiscard]] shared_ptr<Session> get(const SessionID &id);
    [[nodiscard]] shared_ptr<Session> destroy(const SessionID &id);

    /**
     * Append a user turn and enqueue a generation job.
     * @return job key
     */
    [[nodiscard]] optional<JobKey> post_message(const SessionID &id, const SessionMessageRequest &msg);

    /**
     * Client finished running pending tool_calls; append tool results and continue.
     * @return new job key
     */
    [[nodiscard]] optional<JobKey> post_tool_results(const SessionID &id, const SessionToolResultsRequest &body);

    void gc();

  private:
    Jobs &jobs_;
    const ModelAdapter &adapter_;
    std::mutex mutex_;
    std::unordered_map<SessionID, shared_ptr<Session>> sessions_;

    void unsafe_gc();

    optional<JobKey> enqueue_generation(const shared_ptr<Session> &session);

    void on_job_finished(const shared_ptr<Session> &session, shared_ptr<Task> task);
};
