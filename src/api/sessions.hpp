#pragma once

#include "errors.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

using SessionID = uint64_t;
using JobKey = uint64_t;

/**
 * Session state
 */
struct SessionState
{
    enum Value : int32_t
    {
        // Unknown
        Unknown = -1,
        // Waiting for client messages
        Idle,
        // Generating a response
        Generating,
        // Waiting for a tool responses
        AwaitingTools,
        // Waiting for answers of questions sent to the client
        AwaitingQuestion,
    } value{Unknown};

    SessionState() = default;
    SessionState(Value value) : value{value}
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

    [[nodiscard]] bool is_running() const
    {
        return value == Generating;
    }

    [[nodiscard]] bool is_sleeping() const
    {
        return !is_running();
    }

    [[nodiscard]] const char *to_string() const
    {
        switch (value)
        {
        case Idle:
            return "idle";
        case Generating:
            return "generating";
        case AwaitingTools:
            return "awaiting_tools";
        case AwaitingQuestion:
            return "awaiting_question";
        default:
            throw std::runtime_error{std::format("unknown SessionWaitState: {}", std::to_underlying(value))};
        }
    }

    static SessionState to_enum(const std::string &s)
    {
        if (s == "idle")
        {
            return Idle;
        }
        if (s == "generating")
        {
            return  Generating;
        }
        if (s == "awaiting_tools")
        {
            return AwaitingTools;
        }
        if (s == "awaiting_question")
        {
            return AwaitingQuestion;
        }
        throw std::runtime_error{"unknown SessionWaitState: " + s};
    }
};

struct SessionMessageRequest
{
    std::string content;
    std::string role;
    // < 0 inherits the session cap.
    int max_tokens = -1;

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (content.empty())
            throw BadRequest{"property 'content' is required"};
        if (role.empty())
            throw BadRequest{"property 'role' is required"};
    }

    static SessionMessageRequest from_json(const nlohmann::json &j)
    {
        SessionMessageRequest req;
        req.content = j.value("content", "");
        req.role = j.value("role", "user");
        req.max_tokens = j.value("max_tokens", -1);
        req.validate();
        return req;
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        // clang-format off
        return nlohmann::json
        {
            {"content", content},
            {"role", role},
            {"max_tokens", max_tokens},
        };
        // clang-format on
    }
};

struct SessionMessageResponse
{
    SessionID session_id = 0;
    JobKey key = 0;

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (session_id == 0)
            throw BadRequest{"property 'session_id' is required"};
        if (key == 0)
            throw BadRequest{"property 'key' is required"};
    }

    static SessionMessageResponse from_json(const nlohmann::json &j)
    {
        SessionMessageResponse resp;
        resp.session_id = j.value("session_id", SessionID());
        resp.key = j.value("key", JobKey());
        return resp;
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        // clang-format off
        return nlohmann::json
        {
            {"session_id", session_id},
            {"key", key}
        };
        // clang-format on
    }
};

/** NDJSON line for GET /v1/sessions/:id/jobs/:key/tokens */
struct MessageTokensResponse
{
    std::string tokens;
    bool done = false;
    // Present on the terminal line. "done", "error", or "cancelled".
    std::string state;
    std::string error;
    std::string error_code;
    // Session KV tokens and window length. Zero size means the line does not report usage.
    int context_used = 0;
    int context_size = 0;

    [[nodiscard]] nlohmann::json to_json() const
    {
        nlohmann::json j{{"tokens", tokens}, {"done", done}};
        if (!state.empty())
        {
            j["state"] = state;
        }
        if (!error.empty())
        {
            j["error"] = error;
        }
        if (!error_code.empty())
        {
            j["error_code"] = error_code;
        }
        if (context_size > 0)
        {
            j["context_used"] = context_used;
            j["context_size"] = context_size;
        }
        return j;
    }

    static MessageTokensResponse from_json(nlohmann::json &j)
    {
        MessageTokensResponse response;
        response.tokens = j.value("tokens", std::string());
        response.done = j.value("done", false);
        response.state = j.value("state", std::string());
        response.error = j.value("error", std::string());
        response.error_code = j.value("error_code", std::string());
        response.context_used = j.value("context_used", 0);
        response.context_size = j.value("context_size", 0);
        return response;
    }
};
