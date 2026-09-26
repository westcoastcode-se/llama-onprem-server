#include "../sessions/sessions.hpp"
#include "../agent/response_parse.hpp"
#include "../api/errors.hpp"

#include "common/log.hpp"

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
        jobs_.release_session(std::to_string(entry.first));
        return true;
    });
}

shared_ptr<Session> Sessions::create(CreateSessionRequest req)
{
    log_info("starting a new session");

    auto session = std::make_shared<Session>(adapter_);
    session->configure(std::move(req));

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

    if (const auto job = session->active_job())
    {
        jobs_.cancel(*job);
    }
    jobs_.release_session(std::to_string(id));

    return session;
}

void Sessions::on_job_finished(const shared_ptr<Session> &session, shared_ptr<Task> task)
{
    log_info(task, " | is finished");
    session->complete_job(std::move(task));
}

optional<JobKey> Sessions::enqueue_generation(const shared_ptr<Session> &session)
{
    log_info(session, " | queuing up a new generation request for session");

    // Register job key on the session *before* the worker can finish.
    auto task = session->begin_generation();

    std::weak_ptr weak = session;
    task->on_finished = [this, weak](const shared_ptr<Task> &finished) {
        if (auto s = weak.lock())
        {
            on_job_finished(s, finished);
        }
    };

    jobs_.enqueue(task);
    return task->key;
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

    session->accept_user_message(msg);
    return enqueue_generation(session);
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

    session->accept_tool_results(body);
    return enqueue_generation(session);
}

void Sessions::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}
