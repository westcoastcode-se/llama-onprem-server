#pragma once

#include "../agent/response_parse.hpp"

struct ChatMessage
{
    string role;
    string content;

    static ChatMessage from_json(const nlohmann::json &j)
    {
        return ChatMessage{
            .role = j.value("role", "user"),
            .content = j.value("content", ""),
        };
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        return {{"role", role}, {"content", content}};
    }
};

struct MessagesRequest
{
    string system;
    vector<ChatMessage> messages;

    static MessagesRequest from_json(const nlohmann::json &j)
    {
        MessagesRequest req;
        req.system = j.value("system", "");
        auto arr = j.value("messages", nlohmann::json::array());
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

struct MessagesResponse
{
    string key;

    [[nodiscard]] nlohmann::json to_json() const
    {
        return {{"key", key}};
    }
};

struct MessageTokensResponse
{
    string tokens;
    bool done = false;

    [[nodiscard]] nlohmann::json to_json() const
    {
        return {{"tokens", tokens}, {"done", done}};
    }
};

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
