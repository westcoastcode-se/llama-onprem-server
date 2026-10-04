#include "../http/routes.hpp"
#include "../api/errors.hpp"
#include "../api/messages.hpp"
#include "../responses/responses.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <thread>

void send_openai_error(httplib::Response &res, const int status, const std::string_view type, std::string message,
                       const std::string_view code)
{
    nlohmann::json error{{"message", std::move(message)}, {"type", type}};
    if (!code.empty())
    {
        error["code"] = code;
    }
    const nlohmann::json body{{"error", std::move(error)}};
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
    result.tool_namespaces = task.request.tool_namespaces;
    result.tools = task.request.tools;
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
        request.tool_namespaces = std::move(turn.tool_namespaces);
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
}
