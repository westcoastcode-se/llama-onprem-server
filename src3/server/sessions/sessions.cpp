#include "../sessions/sessions.hpp"
#include "../agent/response_parse.hpp"
#include "../api/errors.hpp"
#include <cstdio>
#include <sstream>

#include "common/log.hpp"

Sessions::Sessions(Jobs &jobs) : jobs_(jobs)
{
}

void Sessions::unsafe_gc()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();)
    {
        auto &s = it->second;
        std::lock_guard slock(s->mutex);
        const bool idle = !s->active_task.has_value() && s->state == SessionState::Idle;
        if (idle && now - s->last_active > kIdleTtl)
        {
            it = sessions_.erase(it);
            continue;
        }
        ++it;
    }
}

shared_ptr<Session> Sessions::create(CreateSessionRequest req)
{
    log_info("starting a new session");

    auto session = std::make_shared<Session>();
    session->questions_enabled = req.questions;
    if (req.system.empty())
    {
        session->system_prompt = default_agent_system_prompt("", req.questions);
    }
    else
    {
        session->system_prompt = std::move(req.system);
    }
    session->messages = std::move(req.messages);

    // Add the created session in a thread-safe manner
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

shared_ptr<Session> Sessions::get(const SessionID &id)
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
    log_info("destroying session ", id);

    // Remove the session from the active sessions cache
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

    // And if this session has a job running then cancel it
    if (const auto task = session->get_active_task(); task)
    {
        jobs_.cancel(task.value());
    }

    return session;
}

void Sessions::on_job_finished(const shared_ptr<Session> &session, shared_ptr<Task> task)
{
    std::lock_guard slock(session->mutex);
    log_info(task, " | is finished");
    if (session->active_task != task->key)
    {
        // TODO: Is this actually possible???
        log_error(session, " | was notified by stale ", task);
        return;
    }

    // Session task is now done
    session->active_task.reset();
    session->touch();
    session->latest_finished_task = task;
    if (task->state != JobState::Done)
    {
        session->state = SessionState::Idle;
        session->clear_pending();
        return;
    }

    const string& content = task->result;
    session->messages.push_back(ChatMessage{.role = ChatMessage::ROLE_ASSISTANT, .content = content});

    // Prefer actions already parsed on the task (by Jobs worker)
    ParsedAssistantActions actions;
    {
        // copy under task lock via to_status
        const auto status = task->to_status_unsafe();
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
        session->state = SessionState::AwaitingTools;
    }
    else if (actions.question)
    {
        session->pending_question = std::move(actions.question);
        session->state = SessionState::AwaitingQuestion;
    }
    else
    {
        session->state = SessionState::Idle;
    }
}

optional<JobKey> Sessions::enqueue_generation(const shared_ptr<Session> &session)
{
    log_info(session, " | queuing up a new generation request for session");
    MessagesRequest req;
    {
        std::lock_guard slock(session->mutex);
        req.system = session->system_prompt;
        req.messages = session->messages;
        session->state = SessionState::Generating;
        session->clear_pending();
        session->touch();
    }

    std::weak_ptr weak = session;
    auto key = jobs_.submit(std::move(req), [this, weak](const shared_ptr<Task> &task) {
        if (auto s = weak.lock())
        {
            on_job_finished(s, task);
        }
    });

    // TODO: Refactor these inner scopes with a method on the actual session object instead
    {
        std::lock_guard slock(session->mutex);
        session->active_task = key;
    }

    return key;
}

optional<JobKey> Sessions::post_message(const SessionID &id, const SessionMessageRequest &msg)
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
        if (session->active_task.has_value() || session->state == SessionState::Generating)
        {
            throw Busy("session already has an active generation");
        }
        // New user message supersedes pending tool/question waits
        session->clear_pending();
        session->state = SessionState::Idle;
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

optional<JobKey> Sessions::post_tool_results(const SessionID &id, const SessionToolResultsRequest &body)
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
        if (session->active_task.has_value() || session->state == SessionState::Generating)
        {
            throw Busy("session already has an active generation");
        }
        if (session->state != SessionState::AwaitingTools && session->pending_tool_calls.empty())
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
        session->state = SessionState::Idle;
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
        session->state = SessionState::AwaitingTools;
        return std::nullopt;
    }
    return key;
}

void Sessions::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}
