#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kSubagentName = "sub_agent";
inline constexpr std::string_view kSubagentDescription =
    "Delegate a task or multiple sub-tasks to an isolated sub-agent. The sub-agent runs in an independent "
    "context with full tool access (read/write files, execute commands, search code, web search, etc.) and "
    "returns only its final result. Supports executing a single task or multiple tasks in a controlled "
    "sequential order. Always use sub-agents when exploring large codebases, inspecting multiple or large "
    "files, or investigating complex components to keep large reads out of the parent session. "
    "The result is a summary that cites file:line and does not include file contents, plus git diff --stat when "
    "files changed. "
    "Starts with an empty conversation unless inherit is true. If the result says the context is full, "
    "call again with a smaller task so the answer can be shorter.";
inline constexpr std::string_view kSubagentSchema =
    "arguments:\n"
    "      task: string (one sub-task, or a JSON array of sub-tasks)\n"
    "      tasks: array (optional list of tasks to execute in order)\n"
    "      inherit: boolean (optional, default false; copy this conversation into the sub-agent only when the task needs it)";

Tool create_subagent_tool(SubagentRunner runner);

// The task text shown when the user expands an approval row. Not shortened.
[[nodiscard]] std::string subagent_request_text(const nlohmann::json &args);

// Names a model may emit for this tool. The stored tool_response uses whichever one it called.
inline constexpr std::string_view kSubagentNames[] = {
    kSubagentName,    "subagent",     "delegate_subagent", "delegate_task", "spawn_subagent", "run_subagent",
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
