#pragma once

#include "../common/std.hpp"
#include "sessions.hpp"

#include <format>
#include <utility>

/**
 * Session state
 */
struct JobState
{
    enum Value : int32_t
    {
        Unknown = -1,
        Queued,
        Running,
        Done,
        Error,
        Cancelled
    } value{Unknown};

    JobState() = default;
    JobState(const Value value) : value{value}
    {
    }

    bool operator==(const Value &v) const
    {
        return value == v;
    }

    bool operator != (const Value &v) const
    {
        return value != v;
    }

    /**
     * @return Check to see if the job is in a finished state
     */
    [[nodiscard]] bool is_finished() const
    {
        return value == Done || value == Error || value == Cancelled;
    }

    [[nodiscard]] const char *to_string() const
    {
        switch (value)
        {
        case Queued:
            return "queued";
        case Running:
            return "running";
        case Done:
            return "done";
        case Error:
            return "error";
        case Cancelled:
            return "cancelled";
        default:
            throw std::runtime_error{std::format("unknown JobState: {}", std::to_underlying(value))};
        }
    }

    static JobState to_enum(const string &s)
    {
        if (s == "queued")
        {
            return Queued;
        }
        if (s == "running")
        {
            return  Running;
        }
        if (s == "done")
        {
            return Done;
        }
        if (s == "error")
        {
            return Error;
        }
        if (s == "cancelled")
        {
            return Cancelled;
        }
        throw std::runtime_error{"unknown JobState: " + s};
    }
};


struct ChatTool
{
    string name;
    string description;
    // JSON object. Empty means an object with no properties.
    string parameters;

    [[nodiscard]] json to_json() const
    {
        json parameters_json = json::object();
        if (!parameters.empty())
        {
            parameters_json = json::parse(parameters);
        }
        return json{{"type", "function"},
                    {"function", {{"name", name}, {"description", description}, {"parameters", parameters_json}}}};
    }

    static ChatTool from_json(const json &j)
    {
        const json *fn = &j;
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
            tool.parameters = params.is_string() ? params.get<string>() : params.dump();
            const auto parsed = json::parse(tool.parameters);
            if (!parsed.is_object())
            {
                throw BadRequest{"tool parameters must be a JSON object"};
            }
            tool.parameters = parsed.dump();
        }
        return tool;
    }
};

struct ChatMessage
{
    static constexpr string_view ROLE_ASSISTANT = "assistant";
    static constexpr string_view ROLE_USER = "user";
    static constexpr string_view ROLE_SYSTEM = "system";
    static constexpr string_view ROLE_TOOL = "tool";

    string role;
    string content;
    // Think body kept apart from content so the next turn can round-trip the template.
    string reasoning_content;

    /**
     * Validate required properties. Assistant content may be empty when the turn
     * was only reasoning, or the model stopped before writing an answer.
     */
    void validate() const
    {
        if (role.empty())
            throw BadRequest{"property 'role' is required"};
    }

    static ChatMessage from_json(const nlohmann::json &j)
    {
        ChatMessage message;
        message.role = j.value("role", "user");
        message.content = j.value("content", "");
        message.reasoning_content = j.value("reasoning_content", "");
        message.validate();
        return message;
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        validate();
        json j{{"role", role}, {"content", content}};
        if (!reasoning_content.empty())
        {
            j["reasoning_content"] = reasoning_content;
        }
        return j;
    }
};

/**
 * POST /v1/sessions
 */
struct CreateSessionRequest
{
    // Extra instructions appended to the system prompt the server builds for the active model.
    string system;

    // Chat history, if any exists. This is useful if you want to
    // recreate an old session
    vector<ChatMessage> messages;

    /**
     * When true, the server prompt includes the question protocol and a parsed <question> pauses the session.
     * The client replies with a normal POST .../messages turn.
     */
    bool questions = true;

    // When true, a finished turn's long tool results become one line before the next user message.
    bool compress_tools = true;

    // Model-neutral tools. The server renders them into the system prompt for the loaded model.
    vector<ChatTool> tools;

    // < 0 means the server default (no cap unless LlamaConfig sets one).
    int max_tokens = -1;

    void validate() const
    {
    }

    [[nodiscard]] json to_json() const
    {
        json arr = json::array();
        for (const auto &message : messages)
            arr.push_back(message.to_json());

        json tools_json = json::array();
        for (const auto &tool : tools)
            tools_json.push_back(tool.to_json());

        // clang-format off
        return json
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

    static CreateSessionRequest from_json(const json &j)
    {
        CreateSessionRequest req;
        req.system = j.value("system", "");
        req.questions = j.value("questions", true);
        req.compress_tools = j.value("compress_tools", true);
        req.max_tokens = j.value("max_tokens", -1);
        const auto tools = j.value("tools", json::array());
        if (tools.is_array())
        {
            req.tools.reserve(tools.size());
            for (const auto &tool : tools)
            {
                req.tools.push_back(ChatTool::from_json(tool));
            }
        }
        const auto arr = j.value("messages", json::array());
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
};

/** Structured tool call the model wants the *client* to execute. */
struct ParsedToolCall
{
    string id;
    string name;
    json arguments = json::object();

    [[nodiscard]] json to_json() const
    {
        return {{"id", id}, {"name", name}, {"arguments", arguments}};
    }

    static ParsedToolCall from_json(const json &j)
    {
        ParsedToolCall call;
        call.id = j["id"];
        call.name = j["name"];
        // The server can request any tool, so the argument object stays JSON.
        call.arguments = j["arguments"];
        return call;
    }
};

/** Model asks the user a question with optional multiple-choice answers. */
struct ParsedQuestion
{
    string text;
    vector<string> answers; // options; empty = free-form

    [[nodiscard]] json to_json() const
    {
        json opts = json::array();
        for (const auto &a : answers)
        {
            opts.push_back(a);
        }
        return {{"text", text}, {"answers", opts}};
    }

    static ParsedQuestion from_json(const json &j)
    {
        ParsedQuestion question;
        question.text = j["text"];

        if (const auto arr = j.value("answers", json::array()); arr.is_array())
        {
            question.answers.reserve(arr.size());
            for (const auto &item : arr)
            {
                question.answers.push_back(item.get<string>());
            }
        }
        return question;
    }
};

/** GET /v1/sessions/:id/jobs/:key */
struct MessageStatusResponse
{
    JobKey key = 0;
    JobState state = JobState::Unknown;
    bool done = false;
    string content;
    string error;
    string error_code;
    string reasoning;
    vector<ParsedToolCall> tool_calls;
    optional<ParsedQuestion> question;

    void validate() const
    {
        if (key == 0)
            throw BadRequest{"property 'key' is required"};
        if (state.value == JobState::Unknown)
            throw BadRequest{"property 'state' is required"};
    }

    [[nodiscard]] json to_json() const
    {
        json j{{"key", key}, {"state", state.to_string()}, {"done", done}, {"content", content}};
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

    static MessageStatusResponse from_json(const json &j)
    {
        MessageStatusResponse response;
        response.key = j.value("key", JobKey());
        if (j.contains("state") && j["state"].is_string())
        {
            response.state = JobState::to_enum(j["state"].get<string>());
        }
        response.done = j.value("done", false);
        response.content = j.value("content", string());
        response.error = j.value("error", string());
        response.error_code = j.value("error_code", string());
        response.reasoning = j.value("reasoning_content", string());
        if (const auto arr = j.value("tool_calls", json::array()); arr.is_array())
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
};

/**
 * Response from APIs:
 *
 * 1. POST /v1/sessions
 * 2. GET /v1/sessions/:id
 */
struct SessionResponse
{
    // Unique ID for this session
    SessionID id = 0;
    // The system prompt
    string system_prompt;
    vector<ChatMessage> messages;
    optional<JobKey> active_job_key;
    SessionState state;

    // Pending tool calls
    vector<ParsedToolCall> pending_tool_calls;

    // Pending question
    optional<ParsedQuestion> pending_question;

    // The LLM has asked a question and is waiting for a response.
    // TODO: This should be part of the client and not the server
    bool questions = true;

    // Finished tool results are shortened before the next user message.
    bool compress_tools = true;

    // Set when the latest generation failed. Empty after a successful turn.
    string error;
    // Stable code such as "context_full". Empty when error is empty or unclassified.
    string error_code;

    // Tokens currently stored in this session's KV cache, and the context window length.
    int context_used = 0;
    int context_size = 0;

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (id == 0)
            throw BadRequest{"property 'id' is required"};
        if (state.value == SessionState::Unknown)
            throw BadRequest{"property 'state' is required"};
    }

    [[nodiscard]] json to_json() const
    {
        validate();

        json arr = json::array();
        for (const auto &m : messages)
            arr.push_back(m.to_json());

        // clang-format off
        json j
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
            json tarr = json::array();
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

    static SessionResponse from_json(const json &j)
    {
        SessionResponse req;
        req.id = j.value("id", SessionID());
        req.system_prompt = j.value("system", string());

        if (const auto arr = j.value("messages", json::array()); arr.is_array())
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

        req.state = SessionState::to_enum(j.value("state", string()));

        if (const auto arr = j.value("pending_tool_calls", json::array()); arr.is_array())
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
        req.error = j.value("error", string());
        req.error_code = j.value("error_code", string());
        req.context_used = j.value("context_used", 0);
        req.context_size = j.value("context_size", 0);

        return req;
    }
};
