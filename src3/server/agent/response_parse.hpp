#pragma once

#include "../../common/std.hpp"
#include "../../api/models.hpp"

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

    [[nodiscard]] json to_json() const
    {
        auto j = nlohmann::json::object();
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

/**
 * Parse LLM output for <tool_call(s)>, <question>, <answer> blocks.
 * Tool execution is intentionally left to the client.
 */
ParsedAssistantActions parse_assistant_actions(std::string_view text);

/**
 * Default system prompt: tools, and optionally question/answer protocol.
 *
 * @param extra optional user system text appended at the end
 * @param allow_questions when false, omit <question>/<answer> instructions
 */
string default_agent_system_prompt(std::string_view extra = "", bool allow_questions = true);
