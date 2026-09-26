#pragma once

#include "../../common/std.hpp"
#include "../../api/models.hpp"
#include "model_adapter.hpp"

#include <span>

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
 * Tool-call syntax comes from the model adapter. Tool execution is left to the client.
 * The overload without an adapter uses QwenAdapter.
 */
ParsedAssistantActions parse_assistant_actions(std::string_view text, const ModelAdapter &adapter);
ParsedAssistantActions parse_assistant_actions(std::string_view text);

/**
 * System prompt for the active model. Tool names and parameters come from the client.
 * extra is appended as additional instructions.
 */
string default_agent_system_prompt(const ModelAdapter &adapter, std::span<const ChatTool> tools,
                                   std::string_view extra = "", bool allow_questions = true);
