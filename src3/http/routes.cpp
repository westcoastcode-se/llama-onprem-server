#include "http/routes.hpp"
#include "api/errors.hpp"
#include "api/messages.hpp"
#include "api/sessions.hpp"
#include "json.hpp"

void register_endpoints(httplib::Server &s, AppState &state)
{
    s.Get("/health", [](const httplib::Request &, httplib::Response &res) {
        res.set_content("OK", "text/plain");
    });

    // ---- Sessions (history + job per turn) ----
    s.Post("/v1/sessions", [&state](const httplib::Request &req, httplib::Response &res) {
        try
        {
            nlohmann::json body = nlohmann::json::object();
            if (!req.body.empty())
            {
                body = json::parse(req.body);
            }
            auto created = state.sessions.create(CreateSessionRequest::from_json(body));
            std::lock_guard lock(created->mutex);
            send_json(res, 201, created->to_response());
        }
        catch (const Busy &e)
        {
            res.status = 503;
            res.set_content(error_json("busy", e.what()), "application/json");
        }
        catch (const json::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const std::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
    });

    s.Get("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto session = state.sessions.get(id);
        if (!session)
        {
            res.status = 404;
            res.set_content(error_json("not_found", "session not found"), "application/json");
            return;
        }
        std::lock_guard lock(session->mutex);
        send_json(res, 200, session->to_response());
    });

    s.Delete("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        if (!state.sessions.destroy(id))
        {
            res.status = 404;
            res.set_content(error_json("not_found", "session not found"), "application/json");
            return;
        }
        res.status = 200;
        res.set_content(R"({"deleted":true})", "application/json");
    });

    s.Post("/v1/sessions/:id/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        try
        {
            auto id = req.path_params.at("id");
            auto body = json::parse(req.body);
            auto msg = SessionMessageRequest::from_json(body);
            auto key = state.sessions.post_message(id, std::move(msg));
            if (!key)
            {
                res.status = 503;
                res.set_content(error_json("busy", "job queue is full"), "application/json");
                return;
            }
            send_json(res, 200, SessionMessageResponse{.session_id = id, .key = *key});
        }
        catch (const NotFound &e)
        {
            res.status = 404;
            res.set_content(error_json("not_found", e.what()), "application/json");
        }
        catch (const Busy &e)
        {
            res.status = 503;
            res.set_content(error_json("busy", e.what()), "application/json");
        }
        catch (const BadRequest &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const json::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const std::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
    });

    s.Post("/v1/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        try
        {
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
        }
        catch (const json::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const std::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
    });

    s.Get("/v1/messages/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto task = state.jobs.get_task(id);
        if (!task)
        {
            res.status = 404;
            res.set_content(error_json("not_found", "task not found"), "application/json");
            return;
        }

        const auto st = task->get_state();
        const bool done = st == TaskState::Done || st == TaskState::Error || st == TaskState::Cancelled;

        send_json(res, 200, MessageStatusResponse{
            .key = id,
            .state = to_string(st),
            .done = done,
            .content = done ? task->get_result() : "",
            .error = task->get_error(),
        });
    });

    s.Delete("/v1/messages/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        if (!state.jobs.cancel(id))
        {
            res.status = 404;
            res.set_content(error_json("not_found", "task not found"), "application/json");
            return;
        }
        res.status = 200;
        res.set_content(R"({"cancelled":true})", "application/json");
    });

    // NDJSON stream of token chunks until done
    s.Get("/v1/messages/:id/tokens", [&state](const httplib::Request &req, httplib::Response &res) {
        auto id = req.path_params.at("id");
        auto task = state.jobs.get_task(id);
        if (!task)
        {
            res.status = 404;
            res.set_content(error_json("not_found", "task not found"), "application/json");
            return;
        }

        auto buffer = task->buffer;
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider(
            "application/x-ndjson",
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
