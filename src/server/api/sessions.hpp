#pragma once

#include "../agent/response_parse.hpp"
#include "../api/messages.hpp"
#include "errors.hpp"
#include "../../api/models.hpp"

struct ToolResultItem
{
    std::string id;
    std::string name;
    std::string content;
    // Path, command, query, or URL. Stored on the tool tag so a later one-line record can name the target.
    std::string detail;
    bool denied = false;

    static ToolResultItem from_json(const nlohmann::json &j)
    {
        ToolResultItem item;
        item.id = j.value("id", "");
        item.name = j.value("name", "");
        item.content = j.value("content", j.value("result", ""));
        item.detail = j.value("detail", "");
        item.denied = j.value("denied", false);
        return item;
    }
};

struct SessionToolResultsRequest
{
    std::vector<ToolResultItem> results;

    static SessionToolResultsRequest from_json(const nlohmann::json &j)
    {
        SessionToolResultsRequest req;
        nlohmann::json arr = nlohmann::json::array();
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

