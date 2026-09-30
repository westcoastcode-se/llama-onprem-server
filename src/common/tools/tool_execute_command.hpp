#pragma once

#include "common/tools/tool_types.hpp"

#include <atomic>

namespace Tools {

inline constexpr std::string_view kExecuteCommandName = "execute_command";
inline constexpr std::string_view kExecuteCommandDescription =
    "Execute a shell / bash command on the local system and return the output and exit code.";
inline constexpr std::string_view kExecuteCommandSchema = "arguments:\n      command: string (the shell command to run)";

// First line of a result when the user aborts a running command. The duration follows.
inline constexpr std::string_view kCommandAbortedPrefix = "error: command aborted after ";

Tool create_execute_command_tool();

// Polled while a command runs. The client points this at its Ctrl-C flag.
// Null leaves the command running until it exits or hits the timeout.
void set_execute_command_cancel(const std::atomic<bool> *cancel);

/**
 * @brief Direct execution function for "execute_command".
 * @param args JSON object containing the "command" field.
 * @return String with execution result or error description.
 */
std::string execute_command(const nlohmann::json & args);

} // namespace Tools
