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

} // namespace Tools
