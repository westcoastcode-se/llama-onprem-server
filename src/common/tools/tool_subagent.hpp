#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "sub_agent".
 *
 * Delegates a distinct sub-task to an isolated sub-agent.
 * The sub-agent runs in its own session with full tool access (file read/write,
 * bash commands, web search, etc.) and returns only its final answer.
 * This enables decomposing complex workflows into smaller steps while keeping
 * the main agent's context window clean and compact.
 *
 * JSON Parameters (supports common aliases for robustness):
 *   - task / prompt / instruction / description / subtask / sub_task (string, required).
 *
 * Return value:
 *   - Sub-agent's final answer to the task, or an error message.
 */
Tool create_subagent_tool(SubagentRunner runner);

// The task text shown when the user expands an approval row. Not shortened.
[[nodiscard]] std::string subagent_request_text(const nlohmann::json &args);

// Names a model may emit for this tool. The stored tool_response uses whichever one it called.
inline constexpr std::string_view kSubagentNames[] = {
    "sub_agent",      "subagent",     "delegate_subagent", "delegate_task", "spawn_subagent", "run_subagent",
    "sub_task",       "subtask",      "run_task",          "task_agent",    "tasks",          "run_tasks",
};

[[nodiscard]] inline bool is_subagent_name(std::string_view name)
{
    for (const std::string_view known : kSubagentNames)
    {
        if (known == name)
        {
            return true;
        }
    }
    return false;
}

} // namespace Tools
