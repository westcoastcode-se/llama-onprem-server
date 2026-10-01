#include "../sessions/sessions.hpp"
#include "../agent/response_parse.hpp"
#include "../api/errors.hpp"

#include "common/log.hpp"

Session::Session(const ModelAdapter &adapter) : adapter_(adapter)
{
}

void Session::touch()
{
    std::lock_guard lock(mutex_);
    last_active_ = std::chrono::steady_clock::now();
}

[[nodiscard]] std::chrono::steady_clock::time_point Session::last_active() const
{
    std::lock_guard lock(mutex_);
    return last_active_;
}

[[nodiscard]] std::optional<JobKey> Session::active_job() const
{
    std::lock_guard lock(mutex_);
    return active_job_;
}

[[nodiscard]] std::shared_ptr<Task> Session::latest_finished_job() const
{
    std::lock_guard lock(mutex_);
    return latest_finished_;
}

/** True if this session tracks the job (active or last finished). */
[[nodiscard]] bool Session::owns_job(JobKey job_key) const
{
    std::lock_guard lock(mutex_);
    return tracks_job_unlocked(job_key);
}

/**
 * Resolve a job that belongs to this session.
 * Prefers the live Jobs registry, then falls back to latest finished task.
 */
[[nodiscard]] std::shared_ptr<Task> Session::resolve_job(JobKey job_key, const Jobs &jobs) const
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

[[nodiscard]] SessionResponse Session::to_response() const
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

void Session::set_context_usage(int used, int size)
{
    std::lock_guard lock(mutex_);
    context_used_ = std::max(0, used);
    if (size > 0)
    {
        context_size_ = size;
    }
}

[[nodiscard]] std::pair<int, int> Session::context_usage() const
{
    std::lock_guard lock(mutex_);
    return {context_used_, context_size_};
}

/**
 * Idle long enough to collect. A session waiting on the client is kept:
 * the tool-result or answer POST still has to land on this id.
 */
[[nodiscard]] bool Session::is_gc_idle() const
{
    std::lock_guard lock(mutex_);
    if (active_job_ || state_ == SessionState::Generating)
    {
        return false;
    }
    return state_ != SessionState::AwaitingTools && state_ != SessionState::AwaitingQuestion;
}

[[nodiscard]] Session::Clone Session::capture(std::span<const std::string_view> omit_tools) const
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

void Session::load_clone(Session::Clone clone)
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

void Session::configure(CreateSessionRequest req)
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
void Session::accept_user_message(const SessionMessageRequest &msg)
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
void Session::accept_tool_results(const SessionToolResultsRequest &body)
{
    std::lock_guard lock(mutex_);
    ensure_no_active_generation_unlocked();
    if (state_ != SessionState::AwaitingTools && pending_tool_calls_.empty())
    {
        throw BadRequest("session is not awaiting tool results");
    }

    arm_rollback_unlocked();
    // Leave earlier tool bodies unchanged. Rewriting them changes tokens already in the KV cache.
    // Qwen3.5 cannot drop that suffix, so the whole prompt would be prefilled again.
    messages_.push_back(ChatMessage{.role = std::string(ChatMessage::ROLE_USER),
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
std::shared_ptr<Task> Session::begin_generation()
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
void Session::complete_job(std::shared_ptr<Task> task)
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
    assistant.role = std::string(ChatMessage::ROLE_ASSISTANT);
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

[[nodiscard]] bool Session::tracks_job_unlocked(JobKey job_key) const
{
    if (active_job_ && *active_job_ == job_key)
    {
        return true;
    }
    return latest_finished_ && latest_finished_->key == job_key;
}

void Session::clear_pending_unlocked()
{
    pending_tool_calls_.clear();
    pending_question_.reset();
}

// Tool bodies already in the session become one line. The caller appends the message that stays intact.
void Session::shrink_stored_tool_results_unlocked()
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

void Session::ensure_no_active_generation_unlocked() const
{
    if (active_job_ || state_ == SessionState::Generating)
    {
        throw Busy("session already has an active generation");
    }
}

std::string Session::format_tool_results(const SessionToolResultsRequest &body, bool annotate)
{
    std::string combined;
    for (const auto &r : body.results)
    {
        if (!combined.empty())
        {
            combined.push_back('\n');
        }
        const std::string open = annotate ? tool_response_open(r.name, r.detail) : "<tool_response>";
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

void Session::arm_rollback_unlocked()
{
    rollback_armed_ = true;
    rollback_state_ = state_;
    rollback_tools_ = pending_tool_calls_;
    rollback_question_ = pending_question_;
}

void Session::disarm_rollback_unlocked()
{
    rollback_armed_ = false;
    rollback_tools_.clear();
    rollback_question_.reset();
}

void Session::rollback_last_turn_unlocked()
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

ParsedAssistantActions Session::actions_from_status(const MessageStatusResponse &status) const
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

Sessions::Sessions(Jobs &jobs, const ModelAdapter &adapter) : jobs_(jobs), adapter_(adapter)
{
}

void Sessions::unsafe_gc()
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(sessions_, [&](const auto &entry) {
        const auto &session = entry.second;
        if (!session->is_gc_idle() || now - session->last_active() <= kIdleTtl)
        {
            return false;
        }
        log_info("session ", entry.first, " idle, collected");
        jobs_.release_session(std::to_string(entry.first));
        return true;
    });
}

std::shared_ptr<Session> Sessions::create(CreateSessionRequest req)
{
    log_info("starting a new session");

    auto session = std::make_shared<Session>(adapter_);
    session->configure(std::move(req));
    session->set_context_usage(0, jobs_.context_size());

    {
        std::lock_guard lock(mutex_);
        unsafe_gc();
        if (sessions_.size() >= kMaxSessions)
            throw Busy("too many sessions");
        sessions_[session->id] = session;
    }

    log_info("session ", session->id, " is created");
    return session;
}

std::shared_ptr<Session> Sessions::snapshot(const SessionID &id)
{
    auto parent = get(id);
    if (!parent)
    {
        throw NotFound("session not found");
    }
    if (parent->active_job())
    {
        throw Busy("session is generating");
    }

    static constexpr std::string_view omit[] = {"sub_agent"};
    auto clone = parent->capture(omit);
    auto child = std::make_shared<Session>(adapter_);
    child->load_clone(std::move(clone));
    const auto [used, size] = parent->context_usage();
    child->set_context_usage(used, size > 0 ? size : jobs_.context_size());

    {
        std::lock_guard lock(mutex_);
        unsafe_gc();
        if (sessions_.size() >= kMaxSessions)
        {
            throw Busy("too many sessions");
        }
        sessions_[child->id] = child;
    }

    jobs_.clone_session(std::to_string(parent->id), std::to_string(child->id));
    log_info("session ", parent->id, " snapshotted as ", child->id);
    return child;
}

std::shared_ptr<Session> Sessions::get(const SessionID &id)
{
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end())
    {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<Session> Sessions::destroy(const SessionID &id)
{
    log_info("destroying session ", id);

    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end())
        {
            return {};
        }
        session = it->second;
        sessions_.erase(it);
    }

    if (const auto job = session->active_job())
    {
        jobs_.cancel(*job);
    }
    jobs_.release_session(std::to_string(id));

    return session;
}

void Sessions::on_job_finished(const std::shared_ptr<Session> &session, std::shared_ptr<Task> task)
{
    log_info(task, " | is finished");
    session->complete_job(std::move(task));
}

std::optional<JobKey> Sessions::enqueue_generation(const std::shared_ptr<Session> &session)
{
    log_info(session, " | queuing up a new generation request for session");

    // Register job key on the session *before* the worker can finish.
    auto task = session->begin_generation();

    std::weak_ptr weak = session;
    task->on_finished = [this, weak](const std::shared_ptr<Task> &finished) {
        if (auto s = weak.lock())
        {
            on_job_finished(s, finished);
        }
    };

    jobs_.enqueue(task);
    return task->key;
}

std::optional<JobKey> Sessions::post_message(const SessionID &id, const SessionMessageRequest &msg)
{
    auto session = get(id);
    if (!session)
    {
        throw NotFound("session not found");
    }
    if (msg.content.empty())
    {
        throw BadRequest("content is required");
    }

    session->accept_user_message(msg);
    return enqueue_generation(session);
}

std::optional<JobKey> Sessions::post_tool_results(const SessionID &id, const SessionToolResultsRequest &body)
{
    auto session = get(id);
    if (!session)
    {
        throw NotFound("session not found");
    }
    if (body.results.empty())
    {
        throw BadRequest("tool_results must be a non-empty array");
    }

    session->accept_tool_results(body);
    return enqueue_generation(session);
}

void Sessions::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}
