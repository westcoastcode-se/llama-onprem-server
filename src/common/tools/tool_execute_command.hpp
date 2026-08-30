#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "execute_command".
 *
 * This tool allows the agent to run any shell / bash command on the local system.
 * Standard output and standard error are combined (2>&1) and captured.
 * If the output is large, it is truncated at MAX_TOOL_OUTPUT_CHARS.
 *
 * JSON Parameters:
 *   - command (string, required): The shell command to run.
 *
 * Return value:
 *   - Exit code and full output (or error message).
 */
Tool create_execute_command_tool();

/**
 * @brief Direct execution function for "execute_command".
 * @param args JSON object containing the "command" field.
 * @return String with execution result or error description.
 */
std::string execute_command(const nlohmann::json & args);

} // namespace Tools
