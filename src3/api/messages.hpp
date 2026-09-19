#pragma once

#include "../std.hpp"
#include <string>
#include <vector>

struct ChatMessage
{
    string role;
    string content;

    [[nodiscard]] json to_json() const
    {
        return {{"role", role}, {"content", content}};
    }

    static ChatMessage from_json(const json &j)
    {
        ChatMessage msg;
        msg.role = j.value("role", "");
        msg.content = j.value("content", "");
        return msg;
    }
};

struct MessagesRequest
{
    string system;
    std::vector<ChatMessage> messages;

    [[nodiscard]] json to_json() const
    {
        json arr = json::array();
        for (const auto &m : messages)
        {
            arr.push_back(m.to_json());
        }
        return {{"system", system}, {"messages", arr}};
    }

    static MessagesRequest from_json(const json &j)
    {
        MessagesRequest msg;
        msg.system = j.value("system", "");

        auto arr = j.value("messages", json::array());
        if (arr.is_array())
        {
            msg.messages.reserve(arr.size());
            for (const auto &item : arr)
            {
                msg.messages.push_back(ChatMessage::from_json(item));
            }
        }
        return msg;
    }
};

struct MessagesResponse
{
    string key;

    [[nodiscard]] json to_json() const
    {
        return {{"key", key}};
    }
};

struct MessageStatusResponse
{
    string key;
    string state; // queued|running|done|error|cancelled
    bool done = false;
    string content;
    string error;

    [[nodiscard]] json to_json() const
    {
        json j{{"key", key}, {"state", state}, {"done", done}};
        if (!content.empty())
        {
            j["content"] = content;
        }
        if (!error.empty())
        {
            j["error"] = error;
        }
        return j;
    }
};

struct MessageTokensResponse
{
    vector<string> tokens;
    bool done = false;

    [[nodiscard]] json to_json() const
    {
        json arr = json::array();
        for (const auto &t : tokens)
        {
            arr.push_back(t);
        }
        return {{"tokens", arr}, {"done", done}};
    }
};
