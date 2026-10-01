#pragma once

#include "../jobs/jobs.hpp"
#include "../sessions/sessions.hpp"
#include <httplib.h>

/**
 * State of the application
 */
struct AppState
{
    LlamaEngine &engine;
    Jobs &jobs;
    Sessions &sessions;

    /**
     * Cancel a job that belongs to the given session.
     *
     * @return true if the session owned the job and cancel was requested
     */
    [[nodiscard]] bool try_cancel_job(const SessionID session_id, const JobKey job_key) const
    {
        const auto session = sessions.get(session_id);
        if (!session || !session->owns_job(job_key))
            return false;
        // Only the active job is cancellable; finished jobs are already terminal.
        if (session->active_job() != job_key)
            return false;
        return static_cast<bool>(jobs.cancel(job_key));
    }

    /**
     * Resolve a job that belongs to a session (active or last finished).
     */
    [[nodiscard]] std::shared_ptr<Task> require_session_job(const SessionID session_id, const JobKey job_key) const
    {
        const auto session = sessions.get(session_id);
        if (!session)
            throw NotFound("session not found");

        auto task = session->resolve_job(job_key, jobs);
        if (!task)
            throw NotFound("job not found");
        return task;
    }
};

/**
 *
 * @param server The HTT server
 * @param state Application state
 */
void register_endpoints(httplib::Server &server, AppState &state);
