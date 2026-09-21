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
     * Cancel a job.
     *
     * TODO: The job should be part of the session?
     *
     * @param session_id session id
     * @param job_key The job key
     * @return true if the job exists and it's cancellable
     */
    [[nodiscard]] bool try_cancel_job(const SessionID session_id, const JobKey job_key) const
    {
        auto session = sessions.get(session_id);
        if (!session)
            return false;
        return jobs.cancel(job_key);
    }

    /**
     * Resolve a job that belongs to a session.
     * Accepts the session's active job or any known job key while the session still exists
     * (finished jobs clear active_job_key but remain queryable briefly for status/stream tail).
     */
    std::shared_ptr<Task> require_session_job(const SessionID session_id, const JobKey job_key) const
    {
        auto session = sessions.get(session_id);
        if (!session)
            throw NotFound("session not found");

        auto task = jobs.get_task(job_key);
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
