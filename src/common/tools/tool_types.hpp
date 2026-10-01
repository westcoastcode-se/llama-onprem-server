#pragma once

#include <functional>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Maximum number of characters a tool is allowed to return.
 * If tool output exceeds this limit, it is truncated to avoid overflowing
 * the LLM context window.
 */
inline constexpr size_t MAX_TOOL_OUTPUT_CHARS = 8000;

// Directories omitted from file search and search_text.
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
// One argument as text. Numbers and objects are their JSON form. Missing keys are empty.
[[nodiscard]] inline std::string tool_arg(const nlohmann::json &args, const char *key)
{
    if (!args.is_object() || !args.contains(key) || args[key].is_null())
    {
        return {};
    }
    if (args[key].is_string())
    {
        return args[key].get<std::string>();
    }
    return args[key].dump();
}

// The first key that has text.
[[nodiscard]] inline std::string tool_arg_first(const nlohmann::json &args, std::initializer_list<const char *> keys)
{
    for (const char *key : keys)
    {
        std::string value = tool_arg(args, key);
        if (!value.empty())
        {
            return value;
        }
    }
    return {};
}

struct Tool {
    std::string name;                                        ///< Tool name (e.g. "read_file", "execute_command")
    std::string description;                                 ///< Description of tool purpose/usage for the LLM
    std::string schema_doc;                                  ///< Documentation of tool schema and parameters
    std::function<std::string(const nlohmann::json &)> execute; ///< Callback invoked when the tool is executed
    // Text shown after the name on the collapsed tool line. Empty leaves the name alone.
    std::function<std::string(const nlohmann::json &)> present;
    std::vector<std::string> aliases = {};                   ///< Alternative names / aliases for the tool
};

/**
 * @brief Type alias for a function executing a sub-agent.
 * Takes a task description and returns the sub-agent's final response string.
 */
// inherit is true only when the caller asked to copy the parent conversation.
using SubagentRunner = std::function<std::string(std::string_view task, bool inherit)>;
