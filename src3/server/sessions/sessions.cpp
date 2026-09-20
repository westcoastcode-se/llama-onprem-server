#include "../sessions/sessions.hpp"
#include "../agent/response_parse.hpp"
#include "../api/errors.hpp"
#include <cstdio>
#include <sstream>

Sessions::Sessions(Jobs &jobs) : jobs_(jobs)
{
}

SessionID Sessions::next_id()
{
    return std::to_string(id_counter_.fetch_add(1, std::memory_order_relaxed));
}

void Sessions::unsafe_gc()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto &s = it->second;
        std::lock_guard slock(s->mutex);
        const bool idle = !s->active_job_key.has_value() && s->wait_state == SessionWaitState::Idle;
        if (idle && now - s->last_active > kIdleTtl)
        {
            it = sessions_.erase(it);
            continue;
        }
        ++it;
    }
}

std::shared_ptr<Session> Sessions::create(CreateSessionRequest req)
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
    if (sessions_.size() >= kMaxSessions)
    {
        throw Busy("too many sessions");
    }

    auto session = std::make_shared<Session>();
    session->id = next_id();
    session->questions_enabled = req.questions;
    if (req.system.empty())
    {
        session->system = default_agent_system_prompt("", req.questions);
    }
    else if (req.system.find("<tool_call>") == std::string::npos)
    {
        // Keep user system text, append tool/question protocol if not already present
        session->system = default_agent_system_prompt(req.system, req.questions);
    }
    else
    {
        session->system = std::move(req.system);
    }
    session->messages = std::move(req.messages);

    sessions_[session->id] = session;
    return session;
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

shared_ptr<Session> Sessions::destroy(const SessionID &id)
{
    shared_ptr<Session> session;
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

    optional<Task::Key> job_key;
    {
        std::lock_guard slock(session->mutex);
        job_key = session->active_job_key;
    }

    // If this session has a job running then cancel it
    if (job_key.has_value())
    {
        jobs_.cancel(job_key.value());
    }

    return session;
}

void Sessions::on_job_finished(const std::shared_ptr<Session> &session, const Task &task)
{
    std::lock_guard slock(session->mutex);
    if (session->active_job_key != task.key)
    {
        // Stale callback (e.g. cancelled previous job)
        return;
    }
    session->active_job_key.reset();
    session->touch();

    if (task.get_state() != TaskState::Done)
    {
        session->wait_state = SessionWaitState::Idle;
        session->clear_pending();
        return;
    }

    const std::string content = task.get_result();
    session->messages.push_back(ChatMessage{.role = ChatMessage::ROLE_ASSISTANT, .content = content});

    // Prefer actions already parsed on the task (by Jobs worker)
    ParsedAssistantActions actions;
    {
        // copy under task lock via to_status
        auto status = task.to_status();
        actions.tool_calls = status.tool_calls;
        actions.question = status.question;
        if (actions.tool_calls.empty() && !actions.question)
        {
            actions = parse_assistant_actions(content);
        }
    }

    session->clear_pending();
    // Drop question actions when the session was created without question support
    if (!session->questions_enabled)
    {
        actions.question.reset();
    }
    if (!actions.tool_calls.empty())
    {
        // Tool calls take precedence; client must resolve before question
        session->pending_tool_calls = std::move(actions.tool_calls);
        session->pending_question = std::move(actions.question);
        session->wait_state = SessionWaitState::AwaitingTools;
    }
    else if (actions.question)
    {
        session->pending_question = std::move(actions.question);
        session->wait_state = SessionWaitState::AwaitingQuestion;
    }
    else
    {
        session->wait_state = SessionWaitState::Idle;
    }
}

optional<Task::Key> Sessions::enqueue_generation(const shared_ptr<Session> &session)
{
    MessagesRequest req;
    {
        std::lock_guard slock(session->mutex);
        req.system = session->system;
        req.messages = session->messages;
        session->wait_state = SessionWaitState::Generating;
        session->clear_pending();
        session->touch();
    }

    std::weak_ptr<Session> weak = session;
    auto key = jobs_.submit(std::move(req), [this, weak](const Task &task) {
        if (auto s = weak.lock())
        {
            on_job_finished(s, task);
        }
    });

    if (!key)
    {
        std::lock_guard slock(session->mutex);
        session->wait_state = SessionWaitState::Idle;
        session->active_job_key.reset();
        return std::nullopt;
    }

    {
        std::lock_guard slock(session->mutex);
        session->active_job_key = *key;
    }
    return key;
}

optional<Task::Key> Sessions::post_message(const SessionID &id, const SessionMessageRequest& msg)
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

    {
        std::lock_guard slock(session->mutex);
        if (session->active_job_key.has_value() || session->wait_state == SessionWaitState::Generating)
        {
            throw Busy("session already has an active generation");
        }
        // New user message supersedes pending tool/question waits
        session->clear_pending();
        session->wait_state = SessionWaitState::Idle;
        session->messages.push_back(ChatMessage{.role = msg.role, .content = msg.content});
        session->touch();
    }

    auto key = enqueue_generation(session);
    if (!key)
    {
        std::lock_guard slock(session->mutex);
        if (!session->messages.empty() && session->messages.back().role == msg.role &&
            session->messages.back().content == msg.content)
        {
            session->messages.pop_back();
        }
        return std::nullopt;
    }
    return key;
}

optional<Task::Key> Sessions::post_tool_results(const SessionID &id, const SessionToolResultsRequest& body)
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

    {
        std::lock_guard slock(session->mutex);
        if (session->active_job_key.has_value() || session->wait_state == SessionWaitState::Generating)
        {
            throw Busy("session already has an active generation");
        }
        if (session->wait_state != SessionWaitState::AwaitingTools && session->pending_tool_calls.empty())
        {
            throw BadRequest("session is not awaiting tool results");
        }

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

        session->messages.push_back(ChatMessage{.role = ChatMessage::ROLE_USER, .content = combined.str()});
        session->clear_pending();
        session->wait_state = SessionWaitState::Idle;
        session->touch();
    }

    auto key = enqueue_generation(session);
    if (!key)
    {
        std::lock_guard slock(session->mutex);
        if (!session->messages.empty() && session->messages.back().role == ChatMessage::ROLE_USER &&
            session->messages.back().content.starts_with("<tool_response>"))
        {
            session->messages.pop_back();
        }
        session->wait_state = SessionWaitState::AwaitingTools;
        return std::nullopt;
    }
    return key;
}

void Sessions::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}
