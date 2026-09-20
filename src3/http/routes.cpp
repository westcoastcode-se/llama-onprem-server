#include "http/routes.hpp"
#include "api/errors.hpp"
#include "api/messages.hpp"
#include "api/sessions.hpp"
#include "json.hpp"

/**
 * Register /v1/sessions endpoints
 *
 * @param s
 * @param state
 */
void register_session_endpoints(httplib::Server &s, AppState &state)
{
    // Create a new session
    s.Post("/v1/sessions", [&state](const httplib::Request &req, httplib::Response &res) {
        json body = json::object();
        if (!req.body.empty())
            body = json::parse(req.body);
        const auto created = state.sessions.create(CreateSessionRequest::from_json(body));
        send_json(res, 201, created->to_response());
    });

    // Get all information of the supplied session
    s.Get("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        const auto session = state.sessions.get(id);
        if (!session)
            throw NotFound("session not found");
        send_json(res, 200, session->to_response());
    });

    // Delete a session
    s.Delete("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        if (!state.sessions.destroy(id))
            throw NotFound("session not found");
        res.status = 200;
        res.set_content(R"({"deleted":true})", "application/json");
    });

    // Post a message to a session
    s.Post("/v1/sessions/:id/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto body = json::parse(req.body);
        auto msg = SessionMessageRequest::from_json(body);
        auto key = state.sessions.post_message(id, std::move(msg));
        if (!key)
            throw Busy("job queue is full");
        send_json(res, 200, SessionMessageResponse{.session_id = id, .key = *key});
    });

    // Post tool responses to the active session
    s.Post("/v1/sessions/:id/tools", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto body = json::parse(req.body.empty() ? "{}" : req.body);
        auto key = state.sessions.post_tool_results(id, SessionToolResultsRequest::from_json(body));
        if (!key)
            throw Busy("job queue is full");
        send_json(res, 200, SessionMessageResponse{.session_id = id, .key = *key});
    });
}

void register_endpoints(httplib::Server &s, AppState &state)
{
    s.Get("/health", [](const httplib::Request &, httplib::Response &res) { res.set_content("OK", "text/plain"); });

    register_session_endpoints(s, state);

    //
    // Below are deprecated APIs
    //

    s.Post("/v1/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        auto body = json::parse(req.body);
        auto message = MessagesRequest::from_json(body);

        auto key = state.jobs.submit(std::move(message));
        if (!key)
        {
            res.status = 503;
            res.set_content(error_json("busy", "job queue is full"), "application/json");
            return;
        }

        send_json(res, 200, MessagesResponse{.key = *key});
    });

    s.Get("/v1/messages/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto task = state.jobs.get_task(id);
        if (!task)
            throw NotFound("task not found");

        auto status = task->to_status();
        if (!status.done)
        {
            status.content.clear();
        }
        send_json(res, 200, status);
    });

    s.Delete("/v1/messages/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        if (!state.jobs.cancel(id))
            throw NotFound("task not found");
        res.status = 200;
        res.set_content(R"({"cancelled":true})", "application/json");
    });

    // NDJSON stream of token chunks until done
    s.Get("/v1/messages/:id/tokens", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto task = state.jobs.get_task(id);
        if (!task)
            throw NotFound("task not found");

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
                                                 // EOF
                                                 MessageTokensResponse mm{.tokens = {}, .done = true};
                                                 auto line = mm.to_json().dump() + "\n";
                                                 if (!sink.write(line.data(), line.size()))
                                                 {
                                                     return false;
                                                 }
                                                 sink.done();
                                                 return true;
                                             }

                                             MessageTokensResponse mm{.tokens = {std::move(*piece)}, .done = false};
                                             auto line = mm.to_json().dump() + "\n";
                                             return sink.write(line.data(), line.size());
                                         });
    });
}
