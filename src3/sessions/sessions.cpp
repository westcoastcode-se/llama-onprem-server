#include "sessions/sessions.hpp"
#include "api/errors.hpp"
#include <cstdio>

Sessions::Sessions(Jobs &jobs) : jobs_(jobs)
{
}

Session::Key Sessions::next_id()
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
        const bool idle = s->active_job_key.empty();
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
    session->system = std::move(req.system);
    session->messages = std::move(req.messages);

    sessions_[session->id] = session;
    return session;
}

std::shared_ptr<Session> Sessions::get(const Session::Key &id)
{
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end())
    {
        return {};
    }
    return it->second;
}

bool Sessions::destroy(const Session::Key &id)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end())
        {
            return false;
        }
        session = it->second;
        sessions_.erase(it);
    }

    std::string job_key;
    {
        std::lock_guard slock(session->mutex);
        job_key = session->active_job_key;
        session->active_job_key.clear();
    }
    if (!job_key.empty())
    {
        jobs_.cancel(job_key);
    }
    return true;
}

std::optional<std::string> Sessions::post_message(const Session::Key &id, SessionMessageRequest msg)
{
    if (msg.content.empty())
    {
        throw BadRequest("content is required");
    }
    if (msg.role != "user" && msg.role != "assistant" && msg.role != "system")
    {
        throw BadRequest("invalid role");
    }

    auto session = get(id);
    if (!session)
    {
        throw NotFound("session not found");
    }

    std::string previous_job;
    {
        std::lock_guard slock(session->mutex);
        previous_job = session->active_job_key;
    }

    if (!previous_job.empty())
    {
        auto existing = jobs_.get_task(previous_job);
        if (existing)
        {
            auto st = existing->get_state();
            if (st == TaskState::Queued || st == TaskState::Running)
            {
                throw Busy("session has an active generation; wait or cancel the job first");
            }
        }
    }

    MessagesRequest request;
    {
        std::lock_guard slock(session->mutex);
        // Stale key from a finished job — safe to clear (busy case already handled above).
        if (session->active_job_key == previous_job)
        {
            session->active_job_key.clear();
        }
        else if (!session->active_job_key.empty())
        {
            // Another thread started a job between our checks.
            throw Busy("session has an active generation; wait or cancel the job first");
        }

        session->messages.push_back(ChatMessage{.role = msg.role, .content = msg.content});
        session->touch();

        request.system = session->system;
        request.messages = session->messages;
    }

    auto key = jobs_.submit(std::move(request), [session, weak = std::weak_ptr<Session>(session)](const Task &task) {
        auto s = weak.lock();
        if (!s)
        {
            return;
        }
        std::lock_guard slock(s->mutex);
        if (s->active_job_key != task.key)
        {
            return;
        }
        s->active_job_key.clear();
        s->touch();

        if (task.get_state() != TaskState::Done)
        {
            return;
        }

        const std::string content = task.get_result();
        if (!content.empty())
        {
            s->messages.push_back(ChatMessage{.role = "assistant", .content = content});
        }
    });

    if (!key)
    {
        // Roll back the user message we just appended
        std::lock_guard slock(session->mutex);
        if (!session->messages.empty() && session->messages.back().role == msg.role &&
            session->messages.back().content == msg.content)
        {
            session->messages.pop_back();
        }
        return std::nullopt;
    }

    {
        std::lock_guard slock(session->mutex);
        session->active_job_key = *key;
    }
    return key;
}

void Sessions::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}
