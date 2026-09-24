#include "../http/routes.hpp"
#include "../api/errors.hpp"
#include "../api/messages.hpp"
#include "../api/sessions.hpp"
#include "common/log.hpp"
#include "json.hpp"

namespace
{
void send_job_token_stream(const httplib::Request &req, httplib::Response &res, const std::shared_ptr<Task> &task)
{
    auto buffer = task->buffer;
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider("application/x-ndjson",
                                     [buffer, &req](size_t /*offset*/, httplib::DataSink &sink) {
                                         if (req.is_connection_closed())
                                         {
                                             sink.done();
                                             return false;
                                         }

                                         auto piece = buffer->wait_pull();
                                         if (!piece)
                                         {
                                             MessageTokensResponse mm{.tokens = {}, .done = true};
                                             auto line = mm.to_json().dump();
                                             if (!sink.write(line.data(), line.size()))
                                             {
                                                 return false;
                                             }
                                             sink.done();
                                             return true;
                                         }

                                         MessageTokensResponse mm{.tokens = {std::move(*piece)}, .done = false};
                                         auto line = mm.to_json().dump();
                                         return sink.write(line.data(), line.size());
                                     });
}
} // namespace

/**
 * Register /v1/sessions endpoints
 */
void register_session_endpoints(httplib::Server &s, AppState &state)
{
    // Create a new session
    s.Post("/v1/sessions", [&state](const httplib::Request &req, httplib::Response &res) {
        const auto body = json::parse(req.body.empty() ? "{}" : req.body);
        const auto created = state.sessions.create(CreateSessionRequest::from_json(body));
        log_info(req.remote_addr, ":", req.remote_port, " created ", created);
        send_json(res, 201, created->to_response());
    });

    // Get all information of the supplied session
    s.Get("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const auto session = state.sessions.get(id);
        if (!session)
            throw NotFound("session not found");
        send_json(res, 200, session->to_response());
    });

    // Delete a session
    s.Delete("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const auto session = state.sessions.destroy(id);
        if (!session)
            throw NotFound("session not found");
        log_info(session, " | is destroyed");
        res.status = 200;
        res.set_content(R"({"deleted":true})", "application/json");
    });

    // Post a message to a session
    s.Post("/v1/sessions/:id/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        log_info("Getting messages from session: ", id);
        auto body = json::parse(req.body);
        auto msg = SessionMessageRequest::from_json(body);
        auto key = state.sessions.post_message(id, std::move(msg));
        if (!key)
            throw Busy("job queue is full");
        send_json(res, 200, SessionMessageResponse{.session_id = id, .key = *key});
    });

    // Post tool responses to the active session
    s.Post("/v1/sessions/:id/tools", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        auto key = state.sessions.post_tool_results(id, SessionToolResultsRequest::from_json(body));
        if (!key)
            throw Busy("job queue is full");
        send_json(res, 200, SessionMessageResponse{.session_id = id, .key = *key});
    });

    // Job status for a generation started via this session
    s.Get("/v1/sessions/:id/jobs/:job", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const JobKey job_key = std::stoll(req.path_params.at("job"));
        const auto task = state.require_session_job(id, job_key);
        auto status = task->to_status();
        if (!status.done)
        {
            status.content.clear();
        }
        send_json(res, 200, status);
    });

    // Cancel a job for this session
    s.Delete("/v1/sessions/:id/jobs/:job", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID session_id = std::stoll(req.path_params.at("id"));
        const JobKey job_key = std::stoll(req.path_params.at("job"));
        if (!state.try_cancel_job(session_id, job_key))
            throw NotFound("job not found");
        res.status = 200;
        res.set_content(R"({"cancelled":true})", "application/json");
    });

    // NDJSON stream of token chunks until done
    s.Get("/v1/sessions/:id/jobs/:job/tokens", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const JobKey job_key = std::stoll(req.path_params.at("job"));
        log_info("Streaming tokens from session: ", id, " jobkey: ", job_key);
        auto task = state.require_session_job(id, job_key);
        send_job_token_stream(req, res, task);
    });
}

void register_endpoints(httplib::Server &s, AppState &state)
{
    s.Get("/health", [](const httplib::Request &, httplib::Response &res) { res.set_content("OK", "text/plain"); });

    register_session_endpoints(s, state);
}
