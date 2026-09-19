#pragma once

#include "agent/response_parse.hpp"
#include "api/messages.hpp"
#include "errors.hpp"

struct CreateSessionRequest
{
    string system;
    vector<ChatMessage> messages;
    /** If true (default) and system empty, inject default agent prompt (tools + questions). */
    bool agent = true;

    static CreateSessionRequest from_json(const json &j)
    {
        CreateSessionRequest req;
        req.system = j.value("system", "");
        req.agent = j.value("agent", true);
        auto arr = j.value("messages", json::array());
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
    string id;
    string system;
    vector<ChatMessage> messages;
    string active_job_key; // empty if none
    string state;          // idle|generating|awaiting_tools|awaiting_question
    vector<ParsedToolCall> pending_tool_calls;
    optional<ParsedQuestion> pending_question;

    [[nodiscard]] json to_json() const
    {
        json arr = json::array();
        for (const auto &m : messages)
        {
            arr.push_back(m.to_json());
        }
        json j{{"id", id}, {"system", system}, {"messages", arr}, {"state", state}};
        if (!active_job_key.empty())
        {
            j["active_job_key"] = active_job_key;
        }
        if (!pending_tool_calls.empty())
        {
            json tarr = json::array();
            for (const auto &tc : pending_tool_calls)
            {
                tarr.push_back(tc.to_json());
            }
            j["tool_calls"] = tarr;
        }
        if (pending_question)
        {
            j["question"] = pending_question->to_json();
        }
        return j;
    }
};

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

struct SessionAnswerRequest
{
    string answer;
    /** Optional index into pending question.answers (0-based). */
    optional<int> answer_index;

    static SessionAnswerRequest from_json(const json &j)
    {
        SessionAnswerRequest req;
        req.answer = j.value("answer", j.value("content", ""));
        if (j.contains("answer_index") && j["answer_index"].is_number_integer())
        {
            req.answer_index = j["answer_index"].get<int>();
        }
        else if (j.contains("index") && j["index"].is_number_integer())
        {
            req.answer_index = j["index"].get<int>();
        }
        return req;
    }
};
