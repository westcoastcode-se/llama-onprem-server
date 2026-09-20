#pragma once

#include "../agent/response_parse.hpp"
#include "../../api/models.hpp"

/**
 * Internal generation payload submitted to Jobs (built from a Session).
 * Not exposed as a public REST body — use POST /v1/sessions/:id/messages instead.
 */
struct MessagesRequest
{
    string system;
    vector<ChatMessage> messages;
};

/** NDJSON line for GET /v1/sessions/:id/jobs/:key/tokens */
struct MessageTokensResponse
{
    string tokens;
    bool done = false;

    [[nodiscard]] nlohmann::json to_json() const
    {
        return {{"tokens", tokens}, {"done", done}};
    }
};

/** GET /v1/sessions/:id/jobs/:key */
struct MessageStatusResponse
{
    string key;
    string state; // queued|running|done|error|cancelled
    bool done = false;
    string content;
    string error;
    vector<ParsedToolCall> tool_calls;
    optional<ParsedQuestion> question;

    [[nodiscard]] nlohmann::json to_json() const
    {
        nlohmann::json j{{"key", key}, {"state", state}, {"done", done}, {"content", content}};
        if (!error.empty())
        {
            j["error"] = error;
        }
        if (!tool_calls.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
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
};
