#pragma once

#include "../common/std.hpp"
#include "sessions.hpp"

/**
 * Session state
 */
struct JobState
{
    enum Value : int32_t
    {
        // Unknown
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
            throw std::runtime_error{"unknown JobState: " + std::to_string(value)};
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
    static constexpr string ROLE_ASSISTANT = "assistant";
    static constexpr string ROLE_USER = "user";
    static constexpr string ROLE_SYSTEM = "system";
    static constexpr string ROLE_TOOL = "tool";

    string role;
    string content;

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (role.empty())
            throw BadRequest{"property 'role' is required"};
        if (content.empty())
            throw BadRequest{"property 'content' is required"};
    }

    static ChatMessage from_json(const nlohmann::json &j)
    {
        // clang-format off
        return ChatMessage
        {
            .role = j.value("role", "user"),
            .content = j.value("content", ""),
        };
        // clang-format on
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        validate();

        // clang-format off
        return json
        {
            {"role", role},
            {"content", content}
        };
        // clang-format on
    }
};

/**
 * POST /v1/sessions
 */
struct CreateSessionRequest
{
    // System prompt
    string system;

    // Chat history, if any exists. This is useful if you want to
    // recreate an old session
    vector<ChatMessage> messages;

    /**
     * If true (default), allow question protocol: include it in the default system prompt
     * (when system is empty or lacks tool protocol) and pause on parsed <question> tags.
     * The client replies with a normal POST .../messages turn.
     * When false, questions are omitted from the prompt and ignored if the model still emits them.
     * Tools are available whenever the (effective) system prompt describes them.
     *
     * @deprecated The system prompt should be moved to the client and thus this property is no longer neccessary
     */
    bool questions = true;

    // OpenAI-style function tools rendered into the chat template.
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
            for (const string item : arr)
            {
                question.answers.push_back(item);
            }
        }
        return question;
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
            {"questions", questions}
        };
        // clang-format on

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

        return req;
    }
};
