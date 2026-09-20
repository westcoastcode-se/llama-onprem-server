#pragma once

#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "errors.hpp"
#include "../../api/models.hpp"

struct SessionMessageRequest
{
    string content;
    string role = "user";

    static SessionMessageRequest from_json(const json &j)
    {
        SessionMessageRequest req;
        req.content = j.value("content", "");
        req.role = j.value("role", "user");
        if (req.content.empty())
            throw BadRequest{"property 'content' is required"};
        if (req.role.empty())
            throw BadRequest{"property 'role' is required"};
        return req;
    }
};

struct SessionMessageResponse
{
    string session_id;
    string key; // job key for /v1/messages/:key/tokens

    [[nodiscard]] json to_json() const
    {
        return {{"session_id", session_id}, {"key", key}};
    }
};

struct ToolResultItem
{
    string id;
    string name;
    string content;
    bool denied = false;

    static ToolResultItem from_json(const json &j)
    {
        ToolResultItem item;
        item.id = j.value("id", "");
        item.name = j.value("name", "");
        item.content = j.value("content", j.value("result", ""));
        item.denied = j.value("denied", false);
        return item;
    }
};

struct SessionToolResultsRequest
{
    vector<ToolResultItem> results;

    static SessionToolResultsRequest from_json(const json &j)
    {
        SessionToolResultsRequest req;
        json arr = json::array();
        if (j.contains("tool_results") && j["tool_results"].is_array())
        {
            arr = j["tool_results"];
        }
        else if (j.contains("results") && j["results"].is_array())
        {
            arr = j["results"];
        }
        else if (j.is_array())
        {
            arr = j;
        }
        for (const auto &el : arr)
        {
            req.results.push_back(ToolResultItem::from_json(el));
        }
        return req;
    }
};

