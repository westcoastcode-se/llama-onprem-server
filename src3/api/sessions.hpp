#pragma once

#include "../common/std.hpp"
#include "errors.hpp"

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
            throw std::runtime_error{"unknown SessionWaitState: " + std::to_string(value)};
        }
    }

    static SessionState to_enum(const string &s)
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
    string content;
    string role;

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

    static SessionMessageRequest from_json(const json &j)
    {
        SessionMessageRequest req;
        req.content = j.value("content", "");
        req.role = j.value("role", "user");
        req.validate();
        return req;
    }

    [[nodiscard]] json to_json() const
    {
        // clang-format off
        return json
        {
            {"content", content},
            {"role", role},
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

    static SessionMessageResponse from_json(const json &j)
    {
        SessionMessageResponse resp;
        resp.session_id = j.value("session_id", SessionID());
        resp.key = j.value("key", JobKey());
        return resp;
    }

    [[nodiscard]] json to_json() const
    {
        // clang-format off
        return json
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
    string tokens;
    bool done = false;

    [[nodiscard]] json to_json() const
    {
        return {{"tokens", tokens}, {"done", done}};
    }

    static MessageTokensResponse from_json(json &j)
    {
        return MessageTokensResponse{
            .tokens = j.value("tokens", string()),
            .done = j.value("done", false),
        };
    }
};
