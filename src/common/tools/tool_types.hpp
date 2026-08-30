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
using SubagentRunner = std::function<std::string(std::string_view task)>;
