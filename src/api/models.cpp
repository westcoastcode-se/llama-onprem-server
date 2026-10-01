#include "api/models.hpp"

[[nodiscard]] nlohmann::json ChatTool::to_json() const
{
    nlohmann::json parameters_json = nlohmann::json::object();
    if (!parameters.empty())
    {
        parameters_json = nlohmann::json::parse(parameters);
    }
    return nlohmann::json{{"type", "function"},
                {"function", {{"name", name}, {"description", description}, {"parameters", parameters_json}}}};
}

ChatTool ChatTool::from_json(const nlohmann::json &j)
{
    const nlohmann::json *fn = &j;
    if (j.contains("function") && j.at("function").is_object())
    {
        fn = &j.at("function");
    }
    ChatTool tool;
    tool.name = fn->value("name", "");
    tool.description = fn->value("description", "");
    if (tool.name.empty())
    {
        throw BadRequest{"tool name is required"};
    }
    if (fn->contains("parameters") && !fn->at("parameters").is_null())
    {
        const auto &params = fn->at("parameters");
        tool.parameters = params.is_string() ? params.get<std::string>() : params.dump();
        const auto parsed = nlohmann::json::parse(tool.parameters);
        if (!parsed.is_object())
        {
            throw BadRequest{"tool parameters must be a JSON object"};
        }
        tool.parameters = parsed.dump();
    }
    return tool;
}

ChatMessage ChatMessage::from_json(const nlohmann::json &j)
{
    ChatMessage message;
    message.role = j.value("role", "user");
    message.content = j.value("content", "");
    message.reasoning_content = j.value("reasoning_content", "");
    message.validate();
    return message;
}

[[nodiscard]] nlohmann::json ChatMessage::to_json() const
{
    validate();
    nlohmann::json j{{"role", role}, {"content", content}};
    if (!reasoning_content.empty())
    {
        j["reasoning_content"] = reasoning_content;
    }
    return j;
}

[[nodiscard]] nlohmann::json CreateSessionRequest::to_json() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &message : messages)
        arr.push_back(message.to_json());

    nlohmann::json tools_json = nlohmann::json::array();
    for (const auto &tool : tools)
        tools_json.push_back(tool.to_json());

    // clang-format off
    return nlohmann::json
    {
        {"system", system},
        {"messages", arr},
        {"questions", questions},
        {"compress_tools", compress_tools},
        {"tools", tools_json},
        {"max_tokens", max_tokens},
    };
    // clang-format on
}

CreateSessionRequest CreateSessionRequest::from_json(const nlohmann::json &j)
{
    CreateSessionRequest req;
    req.system = j.value("system", "");
    req.questions = j.value("questions", true);
    req.compress_tools = j.value("compress_tools", true);
    req.max_tokens = j.value("max_tokens", -1);
    const auto tools = j.value("tools", nlohmann::json::array());
    if (tools.is_array())
    {
        req.tools.reserve(tools.size());
        for (const auto &tool : tools)
        {
            req.tools.push_back(ChatTool::from_json(tool));
        }
    }
    const auto arr = j.value("messages", nlohmann::json::array());
    if (arr.is_array())
    {
        req.messages.reserve(arr.size());
        for (const auto &item : arr)
        {
            req.messages.push_back(ChatMessage::from_json(item));
        }
    }
    return req;
}

[[nodiscard]] nlohmann::json ParsedToolCall::to_json() const
{
    return {{"id", id}, {"name", name}, {"arguments", arguments}};
}

ParsedToolCall ParsedToolCall::from_json(const nlohmann::json &j)
{
    ParsedToolCall call;
    call.id = j["id"];
    call.name = j["name"];
    // The server can request any tool, so the argument object stays JSON.
    call.arguments = j["arguments"];
    return call;
}

[[nodiscard]] nlohmann::json ParsedQuestion::to_json() const
{
    nlohmann::json opts = nlohmann::json::array();
    for (const auto &a : answers)
    {
        opts.push_back(a);
    }
    return {{"text", text}, {"answers", opts}};
}

ParsedQuestion ParsedQuestion::from_json(const nlohmann::json &j)
{
    ParsedQuestion question;
    question.text = j["text"];

    if (const auto arr = j.value("answers", nlohmann::json::array()); arr.is_array())
    {
        question.answers.reserve(arr.size());
        for (const auto &item : arr)
        {
            question.answers.push_back(item.get<std::string>());
        }
    }
    return question;
}

[[nodiscard]] nlohmann::json MessageStatusResponse::to_json() const
{
    nlohmann::json j{{"key", key}, {"state", state.to_string()}, {"done", done}, {"content", content}};
    if (!error.empty())
    {
        j["error"] = error;
    }
    if (!error_code.empty())
    {
        j["error_code"] = error_code;
    }
    if (!reasoning.empty())
    {
        j["reasoning_content"] = reasoning;
    }
    if (!tool_calls.empty())
    {
        auto arr = nlohmann::json::array();
        for (const auto &tc : tool_calls)
        {
            arr.push_back(tc.to_json());
        }
        j["tool_calls"] = arr;
    }
    if (question)
    {
        j["question"] = question->to_json();
    }
    return j;
}

MessageStatusResponse MessageStatusResponse::from_json(const nlohmann::json &j)
{
    MessageStatusResponse response;
    response.key = j.value("key", JobKey());
    if (j.contains("state") && j["state"].is_string())
    {
        response.state = JobState::to_enum(j["state"].get<std::string>());
    }
    response.done = j.value("done", false);
    response.content = j.value("content", std::string());
    response.error = j.value("error", std::string());
    response.error_code = j.value("error_code", std::string());
    response.reasoning = j.value("reasoning_content", std::string());
    if (const auto arr = j.value("tool_calls", nlohmann::json::array()); arr.is_array())
    {
        response.tool_calls.reserve(arr.size());
        for (const auto &item : arr)
        {
            response.tool_calls.push_back(ParsedToolCall::from_json(item));
        }
    }
    if (j.contains("question") && j["question"].is_object())
    {
        response.question = ParsedQuestion::from_json(j["question"]);
    }
    return response;
}

[[nodiscard]] nlohmann::json SessionResponse::to_json() const
{
    validate();

    nlohmann::json arr = nlohmann::json::array();
    for (const auto &m : messages)
        arr.push_back(m.to_json());

    // clang-format off
    nlohmann::json j
    {
        {"id", id},
        {"system", system_prompt},
        {"messages", arr},
        {"state", state.to_string()},
        {"questions", questions},
        {"compress_tools", compress_tools},
        {"context_used", context_used},
        {"context_size", context_size}
    };
    // clang-format on

    if (!error.empty())
    {
        j["error"] = error;
    }
    if (!error_code.empty())
    {
        j["error_code"] = error_code;
    }

    if (active_job_key)
    {
        j["active_job_key"] = active_job_key;
    }

    if (!pending_tool_calls.empty())
    {
        nlohmann::json tarr = nlohmann::json::array();
        for (const auto &tc : pending_tool_calls)
        {
            tarr.push_back(tc.to_json());
        }
        j["pending_tool_calls"] = tarr;
    }

    if (pending_question)
    {
        j["pending_question"] = pending_question->to_json();
    }

    return j;
}

SessionResponse SessionResponse::from_json(const nlohmann::json &j)
{
    SessionResponse req;
    req.id = j.value("id", SessionID());
    req.system_prompt = j.value("system", std::string());

    if (const auto arr = j.value("messages", nlohmann::json::array()); arr.is_array())
    {
        req.messages.reserve(arr.size());
        for (const auto &item : arr)
        {
            req.messages.push_back(ChatMessage::from_json(item));
        }
    }

    if (j.contains("active_job_key"))
    {
        req.active_job_key = j.value("active_job_key", JobKey());
    }

    req.state = SessionState::to_enum(j.value("state", std::string()));

    if (const auto arr = j.value("pending_tool_calls", nlohmann::json::array()); arr.is_array())
    {
        req.pending_tool_calls.reserve(arr.size());
        for (const auto &item : arr)
        {
            req.pending_tool_calls.push_back(ParsedToolCall::from_json(item));
        }
    }

    if (j.contains("pending_question"))
    {
        req.pending_question = ParsedQuestion::from_json(j["pending_question"]);
    }

    req.questions = j.value("questions", true);
    req.compress_tools = j.value("compress_tools", true);
    req.error = j.value("error", std::string());
    req.error_code = j.value("error_code", std::string());
    req.context_used = j.value("context_used", 0);
    req.context_size = j.value("context_size", 0);

    return req;
}

