#pragma once

#include "api/messages.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

struct CreateSessionRequest
{
    std::string system;
    std::vector<ChatMessage> messages;

    static CreateSessionRequest from_json(const nlohmann::json &j)
    {
        CreateSessionRequest req;
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

struct SessionResponse
{
    std::string id;
    std::string system;
    std::vector<ChatMessage> messages;
    std::string active_job_key; // empty if none
    std::string state;          // idle|generating

    [[nodiscard]] nlohmann::json to_json() const
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &m : messages)
        {
            arr.push_back(m.to_json());
        }
        nlohmann::json j{{"id", id}, {"system", system}, {"messages", arr}, {"state", state}};
        if (!active_job_key.empty())
        {
            j["active_job_key"] = active_job_key;
        }
        return j;
    }
};

struct SessionMessageRequest
{
    std::string content;
    std::string role = "user";

    static SessionMessageRequest from_json(const nlohmann::json &j)
    {
        SessionMessageRequest req;
        req.content = j.value("content", "");
        req.role = j.value("role", "user");
        return req;
    }
};

struct SessionMessageResponse
{
    std::string session_id;
    std::string key; // job key for /v1/messages/:key/tokens

    [[nodiscard]] nlohmann::json to_json() const
    {
        return {{"session_id", session_id}, {"key", key}};
    }
};
