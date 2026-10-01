#pragma once

#include <filesystem>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Include individual tool definitions and utility modules
#include "common/tools/tool_edit_file.hpp"
#include "common/tools/tool_execute_command.hpp"
#include "common/tools/tool_file_search.hpp"
#include "common/tools/tool_list_directory.hpp"
#include "common/tools/tool_read_file.hpp"
#include "common/tools/tool_search_text.hpp"
#include "common/tools/tool_subagent.hpp"
#include "common/tools/tool_types.hpp"
#include "common/tools/tool_web_fetch.hpp"
#include "common/tools/tool_web_search.hpp"
#include "common/tools/tool_write_file.hpp"

/**
 * @brief Approval status for tool execution.
 */
enum class ToolApproval
{
    ALLOW,  ///< Allow tool execution this time
    DENY,   ///< Deny execution
    ALWAYS, ///< Always allow tool execution during this session
    CLOSED  ///< stdin hit EOF; stop the turn instead of denying and continuing
};

/**
 * @brief Checks if a specific tool is allowed to run automatically based on CLI flags.
 */
bool is_tool_allowed(std::string_view tool_name, bool auto_approve, std::span<const std::string> allowed_tools);

// How the client treats a tool when deciding whether to ask first.
enum class ToolKind
{
    Read,
    Write,
    Shell,
    Network,
    Other
};

[[nodiscard]] ToolKind tool_kind(std::string_view name);

/**
 * Trim the supplied string by removing spaces, newlines and tabs at the start and the end of the string
 *
 * @param text The text to trim
 * @return The trimmed string
 */
constexpr std::string_view string_view_trim(std::string_view text)
{
    constexpr std::string_view exclude = " \n\t\r\0";
    if (const size_t leftShift = text.find_first_not_of(exclude); leftShift != std::string_view::npos)
    {
        text.remove_prefix(leftShift);
    }
    else
    {
        return {};
    }

    if (const size_t rightShift = text.find_last_not_of(exclude); rightShift != std::string_view::npos)
    {
        text.remove_suffix(text.size() - rightShift - 1);
    }
    else
    {
        return {};
    }
    return text;
}

/**
 * @brief Creates and returns the base tool registry (without sub-agents).
 */
std::vector<Tool> get_base_tools();

/**
 * @brief Creates and returns the full registered tool list.
 * @param include_subagents Whether sub-agent tools should be included (default: true).
 * @param subagent_runner Callback to execute sub-agents if enabled.
 */
std::vector<Tool> get_registered_tools(bool include_subagents = true, SubagentRunner subagent_runner = nullptr);

/**
 * @brief Looks up and executes a tool with the specified JSON arguments.
 * Also handles tool aliases and error formatting.
 */
// The tool with this name, or with this alias. Null when nothing matches.
[[nodiscard]] const Tool *find_tool(std::span<const Tool> tools, std::string_view name);

std::string run_tool(std::span<const Tool> tools, std::string_view name, const nlohmann::json &arguments);

/**
 * @brief Loads AGENTS.md from the project root. Empty and missing files yield "".
 */
std::string load_agents_markdown(std::filesystem::path base_dir);
