#pragma once

#include "../std.hpp"

/** Structured tool call the model wants the *client* to execute. */
struct ParsedToolCall
{
    string id;
    string name;
    json arguments = json::object();

    [[nodiscard]] json to_json() const
    {
        return {{"id", id}, {"name", name}, {"arguments", arguments}};
    }
};

/** Model asks the user a question with optional multiple-choice answers. */
struct ParsedQuestion
{
    string text;
    vector<string> answers; // options; empty = free-form

    [[nodiscard]] json to_json() const
    {
        json opts = json::array();
        for (const auto &a : answers)
        {
            opts.push_back(a);
        }
        return {{"text", text}, {"answers", opts}};
    }
};

struct ParsedAssistantActions
{
    vector<ParsedToolCall> tool_calls;
    optional<ParsedQuestion> question;

    /** Visible text with think/tool/question tags stripped (best-effort). */
    string visible_text;

    [[nodiscard]] bool has_client_work() const
    {
        return !tool_calls.empty() || question.has_value();
    }

    [[nodiscard]] nlohmann::json to_json() const
    {
        nlohmann::json j = nlohmann::json::object();
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

/**
 * Parse LLM output for <tool_call(s)>, <question>, <answer> blocks.
 * Tool execution is intentionally left to the client.
 */
ParsedAssistantActions parse_assistant_actions(std::string_view text);

/**
 * Default system prompt: tools, and optionally question/answer protocol.
 * @param extra optional user system text appended at the end
 * @param allow_questions when false, omit <question>/<answer> instructions
 */
std::string default_agent_system_prompt(std::string_view extra = "", bool allow_questions = true);
