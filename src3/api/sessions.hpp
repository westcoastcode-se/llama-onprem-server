#pragma once

#include "../common/std.hpp"
#include "errors.hpp"

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
    string session_id;
    string key; // job key for GET /v1/sessions/:id/jobs/:key/tokens

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (session_id.empty())
            throw BadRequest{"property 'session_id' is required"};
        if (key.empty())
            throw BadRequest{"property 'key' is required"};
    }

    static SessionMessageResponse from_json(const json &j)
    {
        SessionMessageResponse resp;
        resp.session_id = j.value("session_id", "");
        resp.key = j.value("key", "");
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
