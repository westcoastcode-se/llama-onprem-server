#pragma once

#include "../../api/models.hpp"
#include "../agent/response_parse.hpp"

/**
 * Internal generation payload submitted to Jobs (built from a Session).
 * Not exposed as a public REST body — use POST /v1/sessions/:id/messages instead.
 */
struct MessagesRequest
{
    string system;
    vector<ChatMessage> messages;
    string session_id;
    int max_tokens = -1;
    vector<ChatTool> tools;
};

/** GET /v1/sessions/:id/jobs/:key */
struct MessageStatusResponse
{
    JobKey key = 0;
    JobState state = JobState::Unknown;
    bool done = false;
    string content;
    string error;
    vector<ParsedToolCall> tool_calls;
    optional<ParsedQuestion> question;

    /**
     * Validate required properties
     */
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
};
