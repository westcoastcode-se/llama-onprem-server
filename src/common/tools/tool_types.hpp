#pragma once

#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

/**
 * @brief Maximum number of characters a tool is allowed to return.
 * If tool output exceeds this limit, it is truncated to avoid overflowing
 * the LLM context window.
 */
inline constexpr size_t MAX_TOOL_OUTPUT_CHARS = 8000;

// Directories omitted from project search. The map uses the same build and vendor names.
inline bool is_skipped_directory(std::string_view name)
{
    return name == ".git" || name == ".svn" || name == ".hg" || name == ".idea" || name == "node_modules" ||
           name == "vendors" || name == "build" || name == "cmake-build-debug" || name == "cmake-build-release" ||
           name == "cmake-build-verify";
}

/**
 * @brief Representation of an agent tool.
 *
 * Each tool has a unique name, a description shown to the language model,
 * a schema specifying expected arguments, and an execution function
 * that receives JSON arguments and returns the result as a string.
 */
struct Tool {
    std::string name;                                        ///< Tool name (e.g. "read_file", "execute_command")
    std::string description;                                 ///< Description of tool purpose/usage for the LLM
    std::string schema_doc;                                  ///< Documentation of tool schema and parameters
    std::function<std::string(const nlohmann::json &)> execute; ///< Callback invoked when the tool is executed
    std::vector<std::string> aliases = {};                   ///< Alternative names / aliases for the tool
};

/**
 * @brief Type alias for a function executing a sub-agent.
 * Takes a task description and returns the sub-agent's final response string.
 */
// inherit is true only when the caller asked to copy the parent conversation.
using SubagentRunner = std::function<std::string(std::string_view task, bool inherit)>;
