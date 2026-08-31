#pragma once

#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>

// Include individual tool definitions and utility modules
#include "common/tools/tool_types.hpp"
#include "common/tools/tool_execute_command.hpp"
#include "common/tools/tool_read_file.hpp"
#include "common/tools/tool_write_file.hpp"
#include "common/tools/tool_list_directory.hpp"
#include "common/tools/tool_file_search.hpp"
#include "common/tools/tool_search_text.hpp"
#include "common/tools/tool_web_fetch.hpp"
#include "common/tools/tool_web_search.hpp"
#include "common/tools/tool_subagent.hpp"

/**
 * @brief Approval status for tool execution.
 */
enum class ToolApproval {
    ALLOW,   ///< Allow tool execution this time
    DENY,    ///< Deny execution
    ALWAYS   ///< Always allow tool execution during this session
};

/**
 * @brief Parsing result for user input during tool approval prompt.
 */
enum class ToolApprovalParseResult {
    ALLOW,
    DENY,
    ALWAYS,
    INVALID
};

/**
 * @brief Parses user response to approval prompt ('y', 'yes', 'n', 'no', 'a', 'always', etc.).
 */
ToolApprovalParseResult parse_tool_approval_input(std::string_view input);

/**
 * @brief Displays an interactive prompt in the terminal asking user for tool approval.
 */
ToolApproval prompt_tool_approval(std::string_view tool_name, const nlohmann::json & tool_args, std::istream & in = std::cin, std::ostream & out = std::cout);

/**
 * @brief Parses a comma-separated list of allowed tool names.
 */
std::vector<std::string> parse_allowed_tools(std::string_view tools_str);

/**
 * @brief Checks if a specific tool is allowed to run automatically based on CLI flags.
 */
bool is_tool_allowed(std::string_view tool_name, bool auto_approve, std::span<const std::string> allowed_tools);

/**
 * @brief Strips thinking tags (<think>...</think>, <thought>, <reasoning>) from text.
 */
std::string strip_think_tags(std::string_view text);

/**
 * @brief Class containing the various blocks that a response from the server might return, such as
 * <think>...</think>, <tool_calls>...</tool_calls>, <question>...</question>, <answer>...</answer>
 *
 * Please note that the response block is short-lived - basically only available during the actual request-response cycle.
 */
class ResponseBlocks {
public:
    // The thinking block
    std::string_view thinking{};
    // A vector of tool calls that the AI wants to execute
    std::vector<std::string_view> tool_calls{};
    // The text inside a question block - in case the AI want more information from the user
    std::string_view questions{};
    // An array of all answers that the client is allowed to select
    std::vector<std::string_view> answers{};

    static constexpr int thinking_bit = 1 << 0;
    static constexpr int tool_calls_bit = 1 << 1;
    static constexpr int questions_bit = 1 << 2;
    static constexpr int answers_bit = 1 << 3;
    static constexpr int done_bit = 1 << 4;

    // Flags containing information on which blocks was present in the response
    int flags = 0;

    /**
     * @return true if the entire response is done
     */
    [[nodiscard]] bool is_done() const { return flags & done_bit; }

    /**
     * Extract the entire string inside the supplied tag.
     *
     * @param thinking Where to put the result
     * @param text The complete text we've received so far from the server
     * @param tag The tag we are looking for the end of
     * @param pos The position where the tag body content starts
     * @return
     */
    static std::string_view::size_type extract_string(std::string_view& thinking, std::string_view text, std::string_view tag, std::size_t pos);

    /**
     * @param text The text we've received so far from the server
     * @return ResponseBlocks
     */
    static ResponseBlocks from_text(std::string_view text);
};

/**
 * @brief Streaming filter that intercepts and handles thinking tags from LLM output in real time.
 * Suppresses empty thought blocks and routes active thinking tokens to the thinking callback.
 */
class ThinkingStreamFilter {
public:
    using OutputCallback = std::function<void(std::string_view piece, bool is_thinking)>;

    explicit ThinkingStreamFilter(OutputCallback cb);

    void process(std::string_view piece);
    void flush();

private:
    enum class State {
        NORMAL,
        BUFFERING_THINKING,
        STREAMING_THINKING
    };

    OutputCallback cb_;
    State state_ = State::NORMAL;
    std::string buffer_;

    void process_internal();
    void emit_normal(std::string_view piece);
    void emit_thinking(std::string_view piece);
};

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
std::string run_tool(std::span<const Tool> tools, std::string_view name, const nlohmann::json & arguments);

/**
 * @brief Loads AI instructions from the project directory (AI_INSTRUCTIONS.md, etc.).
 */
std::string load_ai_instructions(std::string_view base_dir = ".");

/**
 * @brief Builds the system prompt with environment context, ReAct instructions, and tool schemas.
 */
std::string build_system_prompt(std::span<const Tool> tools, std::string_view custom_prompt = "", std::string_view working_dir = ".");

/**
 * @brief Represents a parsed tool call with tool name and arguments.
 */
struct ToolCall {
    std::string name;
    nlohmann::json arguments;
};

/**
 * @brief Parses all tool calls from LLM output (supporting multiple tool calls/tags or JSON arrays).
 * @param response Raw LLM output.
 * @param tool_calls Output vector of parsed tool calls.
 * @param out_error Optional pointer to receive nlohmann parse error message if JSON parsing fails.
 */
bool parse_tool_calls(std::string_view response, std::vector<ToolCall> & tool_calls, std::string * out_error = nullptr);

/**
 * @brief Parses a single (first) tool call from LLM output (either inside a <tool_call> tag or raw JSON).
 * @param response Raw LLM output.
 * @param name Output tool name.
 * @param arguments Output tool arguments JSON.
 * @param out_error Optional pointer to receive nlohmann parse error message if JSON parsing fails.
 */
bool parse_tool_call(std::string_view response, std::string & name, nlohmann::json & arguments, std::string * out_error = nullptr);
