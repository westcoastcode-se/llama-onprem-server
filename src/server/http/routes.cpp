#include "../http/routes.hpp"
#include "../api/errors.hpp"
#include "../api/messages.hpp"
#include "../api/sessions.hpp"
#include "../responses/responses.hpp"
#include "common/log.hpp"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

namespace
{
void send_job_token_stream(const httplib::Request &, httplib::Response &res, const std::shared_ptr<Task> &task)
{
    auto buffer = task->buffer;
    res.set_header("Cache-Control", "no-cache");
    auto connection_closed = std::make_shared<std::atomic<bool>>(false);
    res.set_chunked_content_provider(
        "application/x-ndjson",
        [buffer, connection_closed, task](size_t /*offset*/, httplib::DataSink &sink) {
            if (connection_closed->load(std::memory_order_relaxed))
            {
                sink.done();
                return false;
            }

            auto piece = buffer->wait_pull();
            if (!piece)
            {
                const auto status = task->to_status();
                MessageTokensResponse mm;
                mm.done = true;
                mm.state = status.state.to_string();
                mm.error = status.error;
                mm.error_code = status.error_code;
                mm.context_used = task->context_used();
                mm.context_size = task->context_size();
                auto line = mm.to_json().dump() + "\n";
                if (!sink.write(line.data(), line.size()))
                {
                    connection_closed->store(true, std::memory_order_relaxed);
                    return false;
                }
                sink.done();
                return true;
            }

            MessageTokensResponse mm{
                .tokens = {std::move(*piece)},
                .done = false,
                .state = {},
                .error = {},
                .error_code = {},
                .context_used = task->context_used(),
                .context_size = task->context_size(),
            };
            auto line = mm.to_json().dump() + "\n";
            if (!sink.write(line.data(), line.size()))
            {
                connection_closed->store(true, std::memory_order_relaxed);
                return false;
            }
            return true;
        });
}
} // namespace

void send_openai_error(httplib::Response &res, const int status, const std::string_view type, std::string message)
{
    const nlohmann::json body{{"error", {{"message", std::move(message)}, {"type", type}}}};
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void send_json_raw(httplib::Response &res, const int status, const nlohmann::json &body)
{
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

ResponsesResult result_from_task(const Task &task)
{
    ResponsesResult result;
    const auto status = task.to_status();
    if (status.state == JobState::Cancelled)
    {
        result.status = ResponsesResult::Status::Cancelled;
        return result;
    }
    if (status.state == JobState::Error)
    {
        result.status = ResponsesResult::Status::Failed;
        result.error = status.error;
        result.error_code = status.error_code;
        return result;
    }
    result.reasoning = status.reasoning;
    result.text = responses_visible_text(status.content);
    result.tool_calls = status.tool_calls;
    return result;
}

void wait_until_finished(const std::shared_ptr<Task> &task)
{
    while (!task->get_state().is_finished())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void enqueue_delta_frames(std::deque<std::string> &frames, const ResponseIds &ids, ResponsesDeltaFilter::Delta delta,
                          bool &saw_reasoning, bool &saw_text)
{
    if (!delta.reasoning.empty())
    {
        if (!saw_reasoning)
        {
            saw_reasoning = true;
            frames.push_back(sse_frame({
                {"type", "response.output_item.added"},
                {"item",
                 {{"id", ids.reasoning},
                  {"type", "reasoning"},
                  {"status", "in_progress"},
                  {"summary", nlohmann::json::array()},
                  {"content", nlohmann::json::array()},
                  {"encrypted_content", ""}}},
            }));
        }
        frames.push_back(sse_frame({
            {"type", "response.reasoning_text.delta"},
            {"item_id", ids.reasoning},
            {"content_index", 0},
            {"delta", std::move(delta.reasoning)},
        }));
    }
    if (!delta.text.empty())
    {
        if (!saw_text)
        {
            saw_text = true;
            frames.push_back(sse_frame({
                {"type", "response.output_item.added"},
                {"item",
                 {{"id", ids.message},
                  {"type", "message"},
                  {"role", "assistant"},
                  {"status", "in_progress"},
                  {"content", nlohmann::json::array()}}},
            }));
        }
        frames.push_back(sse_frame({
            {"type", "response.output_text.delta"},
            {"item_id", ids.message},
            {"delta", std::move(delta.text)},
        }));
    }
}

void stream_responses(httplib::Response &res, Jobs &jobs, std::shared_ptr<Task> task, std::string model)
{
    struct Stream
    {
        std::shared_ptr<Task> task;
        Jobs *jobs = nullptr;
        std::string model;
        ResponseIds ids = ResponseIds::make();
        ResponsesDeltaFilter filter;
        std::deque<std::string> frames;
        bool pulling = false;
        bool terminal = false;
        bool saw_reasoning = false;
        bool saw_text = false;
    };
    auto stream = std::make_shared<Stream>();
    stream->task = std::move(task);
    stream->jobs = &jobs;
    stream->model = std::move(model);

    res.set_header("Cache-Control", "no-cache");
    auto closed = std::make_shared<std::atomic<bool>>(false);
    res.set_chunked_content_provider("text/event-stream", [stream, closed](size_t, httplib::DataSink &sink) {
        if (closed->load(std::memory_order_relaxed))
        {
            sink.done();
            return false;
        }
        // Tool XML and a held marker prefix produce no SSE frame. Ending the body
        // on that pull makes Codex report "stream closed before response.completed".
        while (stream->frames.empty() && !stream->terminal)
        {
            if (!stream->pulling)
            {
                stream->pulling = true;
                stream->frames.push_back(sse_frame({
                    {"type", "response.created"},
                    {"response", {{"id", stream->ids.response}, {"object", "response"}, {"status", "in_progress"}}},
                }));
                stream->frames.push_back(sse_frame({
                    {"type", "response.in_progress"},
                    {"response", {{"id", stream->ids.response}, {"object", "response"}, {"status", "in_progress"}}},
                }));
            }
            else
            {
                auto piece = stream->task->buffer->wait_pull();
                if (!piece)
                {
                    enqueue_delta_frames(stream->frames, stream->ids, stream->filter.finish(), stream->saw_reasoning,
                                         stream->saw_text);
                    wait_until_finished(stream->task);
                    for (const auto &event : response_terminal_events(stream->ids, stream->model, result_from_task(*stream->task)))
                    {
                        stream->frames.push_back(sse_frame(event));
                    }
                    stream->terminal = true;
                }
                else
                {
                    enqueue_delta_frames(stream->frames, stream->ids, stream->filter.feed(*piece), stream->saw_reasoning,
                                         stream->saw_text);
                }
            }
        }
        if (stream->frames.empty())
        {
            sink.done();
            return true;
        }
        const auto &frame = stream->frames.front();
        if (!sink.write(frame.data(), frame.size()))
        {
            closed->store(true, std::memory_order_relaxed);
            stream->jobs->cancel(stream->task->key);
            return false;
        }
        stream->frames.pop_front();
        return true;
    });
}

void handle_responses(const httplib::Request &req, httplib::Response &res, Jobs &jobs, const std::string &loaded_model)
{
    try
    {
        const auto body = nlohmann::json::parse(req.body.empty() ? "" : req.body);
        auto turn = responses_from_json(body);
        const std::string model = turn.model.empty() ? loaded_model : turn.model;
        MessagesRequest request;
        request.system = std::move(turn.instructions);
        request.messages = std::move(turn.messages);
        request.tools = std::move(turn.tools);
        request.max_tokens = turn.max_tokens;
        request.temperature = turn.temperature;
        request.session_id = std::move(turn.session_id);
        const bool stream = turn.stream;

        if (!stream)
        {
            auto ready = std::make_shared<std::atomic<bool>>(false);
            auto task = jobs.submit(std::move(request), [ready](const std::shared_ptr<Task> &) {
                ready->store(true, std::memory_order_release);
            });
            while (!ready->load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto ids = ResponseIds::make();
            const auto result = result_from_task(*task);
            if (result.status == ResponsesResult::Status::Failed)
            {
                send_openai_error(res, 500, "server_error", result.error.empty() ? "generation failed" : result.error);
                return;
            }
            const auto events = response_terminal_events(ids, model, result);
            send_json_raw(res, 200, events.back().at("response"));
            return;
        }

        auto task = jobs.submit(std::move(request));
        stream_responses(res, jobs, std::move(task), model);
    }
    catch (const BadRequest &error)
    {
        send_openai_error(res, 400, "invalid_request_error", error.what());
    }
    catch (const nlohmann::json::exception &)
    {
        send_openai_error(res, 400, "invalid_request_error", "request body is not valid JSON");
    }
}

/**
 * Register /v1/sessions endpoints
 */
void register_session_endpoints(httplib::Server &s, AppState &state)
{
    // Create a session. A body with id resumes that session instead.
    s.Post("/v1/sessions", [&state](const httplib::Request &req, httplib::Response &res) {
        const auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        const auto request = CreateSessionRequest::from_json(body);
        const auto created = state.sessions.create(request);
        if (request.id != 0)
        {
            log_info(req.remote_addr, ":", req.remote_port, " resumed ", created);
            send_json(res, 200, created->to_response());
            return;
        }
        log_info(req.remote_addr, ":", req.remote_port, " created ", created);
        send_json(res, 201, created->to_response());
    });

    // Headers only. The transcript of one session is GET /v1/sessions/:id/messages.
    s.Get("/v1/sessions", [&state](const httplib::Request &, httplib::Response &res) {
        SessionListResponse body;
        const auto sessions = state.sessions.list();
        body.sessions.reserve(sessions.size());
        for (const auto &session : sessions)
        {
            body.sessions.push_back(session->to_response(false));
        }
        send_json(res, 200, body);
    });

    // Session header. The transcript is GET /v1/sessions/:id/messages.
    s.Get("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const auto session = state.sessions.get(id);
        if (!session)
            throw NotFound("session not found");
        send_json(res, 200, session->to_response(false));
    });

    // Delete a session
    s.Delete("/v1/sessions/:id", [&state](const httplib::Request &req, httplib::Response &res) {
        const auto session_id = static_cast<SessionID>(std::stoll(req.path_params.at("id")));
        const auto session = state.sessions.destroy(session_id);
        if (!session)
            throw NotFound("session not found");
        log_info(session, " | is destroyed");
        res.status = 200;
        res.set_content(R"({"deleted":true})", "application/json");
    });

    // Copy the session's conversation into a new session for a sub-agent.
    s.Post("/v1/sessions/:id/snapshots", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const auto child = state.sessions.snapshot(id);
        log_info(req.remote_addr, ":", req.remote_port, " snapshotted ", id, " as ", child->id);
        send_json(res, 201, child->to_response());
    });

    s.Get("/v1/sessions/:id/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        const auto session = state.sessions.get(id);
        if (!session)
        {
            throw NotFound("session not found");
        }
        // Opening the transcript is how a client resumes. The new time keeps this session
        // when the on-disk cache is over its size limit.
        state.sessions.note_resume(id);
        SessionMessagesResponse body;
        body.messages = session->to_response().messages;
        send_json(res, 200, body);
    });

    // Post a message to a session
    s.Post("/v1/sessions/:id/messages", [&state](const httplib::Request &req, httplib::Response &res) {
        const auto session_id = static_cast<SessionID>(std::stoll(req.path_params.at("id")));
        log_info("Getting messages from session: ", session_id);
        const auto body = nlohmann::json::parse(req.body);
        const auto msg = SessionMessageRequest::from_json(body);
        const auto key = state.sessions.post_message(session_id, msg);
        if (!key)
            throw Busy("job queue is full");
        send_json(res, 200, SessionMessageResponse{.session_id = session_id, .key = *key});
    });

    // Post tool responses to the active session
    s.Post("/v1/sessions/:id/tools", [&state](const httplib::Request &req, httplib::Response &res) {
        const SessionID id = std::stoll(req.path_params.at("id"));
        auto body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
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
        const auto session_id = static_cast<SessionID>(std::stoll(req.path_params.at("id")));
        const JobKey job_key = std::stoll(req.path_params.at("job"));
        log_info("Streaming tokens from session: ", session_id, " job: ", job_key);
        const auto task = state.require_session_job(session_id, job_key);
        send_job_token_stream(req, res, task);
    });
}

void register_endpoints(httplib::Server &s, AppState &state)
{
    s.Get("/health", [](const httplib::Request &, httplib::Response &res) { res.set_content("OK", "text/plain"); });

    const std::string model = public_model_id(state.engine.get_config().model_path);
    const auto models = [model](const httplib::Request &, httplib::Response &res) {
        send_json_raw(res, 200,
                      {{"object", "list"},
                       {"data", nlohmann::json::array({{{"id", model}, {"object", "model"}, {"owned_by", "callisto"}}})}});
    };
    s.Get("/v1/models", models);
    s.Get("/models", models);

    const auto responses = [&state, model](const httplib::Request &req, httplib::Response &res) {
        handle_responses(req, res, state.jobs, model);
    };
    s.Post("/v1/responses", responses);
    s.Post("/responses", responses);

    register_session_endpoints(s, state);
}
