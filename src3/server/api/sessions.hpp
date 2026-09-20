#pragma once

#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "errors.hpp"
#include "../../api/models.hpp"

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

