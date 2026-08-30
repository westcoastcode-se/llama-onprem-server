#include "common/tools.hpp"
#include "common/color.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

// ---------------------------------------------------------------------------
// Tool Registry Setup
// ---------------------------------------------------------------------------

std::vector<Tool> get_base_tools() {
    // Returns built-in base tools for the agent
    return {
        Tools::create_execute_command_tool(),
        Tools::create_read_file_tool(),
        Tools::create_write_file_tool(),
        Tools::create_list_directory_tool(),
        Tools::create_file_search_tool(),
        Tools::create_search_text_tool(),
        Tools::create_web_fetch_tool(),
        Tools::create_web_search_tool()
    };
}

std::vector<Tool> get_registered_tools(bool include_subagents, SubagentRunner subagent_runner) {
    std::vector<Tool> tools = get_base_tools();
    if (include_subagents) {
        // Add sub-agent tool if sub-agents are enabled
        tools.push_back(Tools::create_subagent_tool(subagent_runner));
    }
    return tools;
}

// ---------------------------------------------------------------------------
// Environment & Prompt Construction
// ---------------------------------------------------------------------------

static std::string get_current_iso_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

std::string load_ai_instructions(std::string_view base_dir) {
    std::filesystem::path dir = base_dir.empty() ? std::filesystem::current_path() : std::filesystem::path(base_dir);

    // Search for standard project instruction files in priority order
    const std::vector<std::string> candidate_names = {
        "AI_INSTRUCTIONS.md",
        "ai_instructions.md",
        ".github/copilot-instructions.md",
        ".github/copilot_instructions.md",
        "copilot_instructions.md",
        "copilot-instructions.md",
        ".copilot_instructions.md",
        ".copilot-instructions.md",
        ".ai_instructions.md"
    };

    for (const auto & rel : candidate_names) {
        std::error_code ec;
        std::filesystem::path p = dir / rel;
        if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
            std::ifstream ifs(p);
            if (ifs.is_open()) {
                std::stringstream buffer;
                buffer << ifs.rdbuf();
                std::string content = buffer.str();
                // Trim trailing whitespace
                size_t last = content.find_last_not_of(" \t\r\n");
                if (last != std::string::npos) {
                    content = content.substr(0, last + 1);
                } else {
                    content.clear();
                }
                if (!content.empty()) {
                    return content;
                }
            }
        }
    }
    return "";
}

std::string build_system_prompt(std::span<const Tool> tools, std::string_view custom_prompt, std::string_view working_dir) {
    std::string cwd = working_dir.empty() || working_dir == "." ? std::filesystem::current_path().string() : std::string(working_dir);
    std::string now_str = get_current_iso_time();

    bool has_subagents = false;
    for (const auto & tool : tools) {
        if (tool.name == "sub_agent") {
            has_subagents = true;
            break;
        }
    }

    std::ostringstream ss;
    ss << "You are an intelligent, autonomous AI agent equipped with tools to solve complex tasks directly on the user's system.\n\n";
    ss << "## Environment Context:\n";
    ss << "- Operating System: Linux\n";
    ss << "- Working Directory: " << cwd << "\n";
    ss << "- Current Date & Time: " << now_str << "\n\n";

    ss << "## ReAct Agent Loop (Reasoning + Action):\n";
    if (has_subagents) {
        ss << "1. **Thought / Plan (Planning Phase)**: Analyze the user's request. For complex problems, multi-step goals, exploring large codebases, or when working across components (e.g., client, server, fat_client), decompose the problem into distinct, modular tasks during this planning phase. Determine which tasks to delegate to sub-agents to optimize context and keep the main context clean and modular.\n";
    } else {
        ss << "1. **Thought / Plan**: Analyze the user's goal, break it down into logical steps, and determine if an action/tool is needed.\n";
    }
    ss << "2. **Action**: Select the appropriate tool and output a tool call wrapped in <tool_call> tags. When planning multiple tasks or actions, emit the required tool calls sequentially.\n";
    ss << "3. **Observation**: Wait for the tool execution response (<tool_response>). All executed tasks and actions are returned in controlled order.\n";
    ss << "4. **Iterate**: Repeat Thought -> Action -> Observation until all tasks are resolved.\n";
    ss << "5. **Final Answer**: Once you have completed the goal or answered the question, present a clear, comprehensive final response directly to the user (without any tool call).\n\n";

    if (has_subagents) {
        ss << "## Sub-Agent Task Planning, Context Optimization & Delegation:\n";
        ss << "Sub-agents are enabled (`sub_agent` tool). In the **planning phase** (Thought/Plan) and execution:\n";
        ss << "- **Context Optimization & Delegation**: Always use sub-agents whenever needed when working in large projects or across client, server, and fat_client components. Sub-agents run in isolated contexts, preventing heavy file contents, directory listings, or verbose logs from overflowing the main conversation context.\n";
        ss << "- **Reusing Sub-Agent Results**: Use a sub-agent's summarized output and findings in subsequent steps or other sub-agents so that you can continue effectively without having to re-read large files from scratch.\n";
        ss << "- **Task Decomposition**: When tackling complex problems or when asked to break down work into tasks, break the problem down into sequential or focused sub-tasks and execute each sub-task by calling `sub_agent` with clear, self-contained instructions. If multiple tasks are passed, they will all be executed in sequential, controlled order.\n";
        ss << "- **Follow-up Execution**: Every sub-agent's response and findings can also directly result in executing tools (e.g., executing commands, writing/editing files, reading files, searching, or launching subsequent sub-agents/sub-tasks). Act on the sub-agent's findings by running any necessary follow-up tools.\n";
        ss << "- **Synthesis**: Combine and summarize the final results from all sub-agents in your final answer.\n\n";
    }

    ss << "## Tool Calling Format:\n";
    ss << "To invoke a tool, output valid JSON inside a tool_call XML tag exactly:\n";
    ss << "<tool_call>\n";
    ss << "{\n";
    ss << "  \"name\": \"<tool_name>\",\n";
    ss << "  \"arguments\": {\n";
    ss << "    \"<param>\": <value>\n";
    ss << "  }\n";
    ss << "}\n";
    ss << "</tool_call>\n\n";

    ss << "## Available Tools:\n";
    for (const auto & tool : tools) {
        ss << "- **" << tool.name << "**:\n";
        ss << "    description: " << tool.description << "\n";
        ss << "    " << tool.schema_doc << "\n\n";
    }

    std::string project_instructions = load_ai_instructions(working_dir);
    if (!project_instructions.empty()) {
        ss << "## Project Instructions (AI_INSTRUCTIONS.md):\n" << project_instructions << "\n\n";
    }

    if (!custom_prompt.empty()) {
        ss << "## Additional Instructions:\n" << custom_prompt << "\n";
    }

    return ss.str();
}

// ---------------------------------------------------------------------------
// Tool Dispatcher & Execution
// ---------------------------------------------------------------------------

std::string run_tool(std::span<const Tool> tools, std::string_view name, const nlohmann::json & arguments) {
    // 1. Exact match by tool name
    for (const auto & tool : tools) {
        if (tool.name == name) {
            try {
                return tool.execute(arguments);
            } catch (const std::exception & e) {
                return std::string("error executing tool '") + std::string(name) + "': " + e.what();
            }
        }
    }
    // 2. Match against tool aliases
    for (const auto & tool : tools) {
        for (const auto & alias : tool.aliases) {
            if (alias == name) {
                try {
                    return tool.execute(arguments);
                } catch (const std::exception & e) {
                    return std::string("error executing tool '") + std::string(name) + "': " + e.what();
                }
            }
        }
    }
    return std::string("error: unknown tool '") + std::string(name) + "'";
}

// ---------------------------------------------------------------------------
// Tool Call Parsing
// ---------------------------------------------------------------------------

static void sanitize_json(std::string & s) {
    bool in_str = false;
    bool esc = false;
    size_t last_comma_pos = std::string::npos;

    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }

        if (c == '"') {
            in_str = true;
            last_comma_pos = std::string::npos;
        } else if (c == ',') {
            last_comma_pos = i;
        } else if (c == '}' || c == ']') {
            if (last_comma_pos != std::string::npos) {
                s[last_comma_pos] = ' ';
                last_comma_pos = std::string::npos;
            }
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            last_comma_pos = std::string::npos;
        }
    }
}

static std::string escape_control_chars_in_strings(const std::string & s) {
    std::string res;
    res.reserve(s.size() + 16);
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
                res.push_back(c);
            } else if (c == '\\') {
                esc = true;
                res.push_back(c);
            } else if (c == '"') {
                in_str = false;
                res.push_back(c);
            } else if (c == '\n') {
                res.append("\\n");
            } else if (c == '\r') {
                res.append("\\r");
            } else if (c == '\t') {
                res.append("\\t");
            } else {
                res.push_back(c);
            }
        } else {
            if (c == '"') {
                in_str = true;
            }
            res.push_back(c);
        }
    }
    return res;
}

static bool try_parse_json_lenient(std::string_view json_str, nlohmann::json & out_json, std::string * out_error = nullptr) {
    std::string first_err;
    try {
        out_json = nlohmann::json::parse(json_str);
        if (out_error) out_error->clear();
        return true;
    } catch (const std::exception & e) {
        first_err = e.what();
    }

    std::string s(json_str);
    sanitize_json(s);
    try {
        out_json = nlohmann::json::parse(s);
        if (out_error) out_error->clear();
        return true;
    } catch (...) {}

    std::string escaped = escape_control_chars_in_strings(s);
    try {
        out_json = nlohmann::json::parse(escaped);
        if (out_error) out_error->clear();
        return true;
    } catch (...) {}

    if (out_error && out_error->empty() && !first_err.empty()) {
        *out_error = std::move(first_err);
    }
    return false;
}

static bool try_extract_and_parse_json(std::string_view text, size_t from, char start_char, nlohmann::json & out_json, size_t & out_end_pos, std::string * out_error = nullptr) {
    const size_t begin = text.find(start_char, from);
    if (begin == std::string_view::npos) {
        return false;
    }

    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = begin; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            stack.push_back('{');
        } else if (c == '[') {
            stack.push_back('[');
        } else if (c == '}') {
            if (!stack.empty() && stack.back() == '{') {
                stack.pop_back();
                if (stack.empty()) {
                    std::string_view balanced = text.substr(begin, i - begin + 1);
                    if (try_parse_json_lenient(balanced, out_json, out_error)) {
                        out_end_pos = i + 1;
                        return true;
                    }
                }
            }
        } else if (c == ']') {
            if (!stack.empty() && stack.back() == '[') {
                stack.pop_back();
                if (stack.empty()) {
                    std::string_view balanced = text.substr(begin, i - begin + 1);
                    if (try_parse_json_lenient(balanced, out_json, out_error)) {
                        out_end_pos = i + 1;
                        return true;
                    }
                }
            }
        }
    }

    // If balanced extraction didn't complete, attempt to repair unclosed structure
    if (!stack.empty() || in_string) {
        std::string repaired(text.substr(begin));
        if (in_string) {
            while (!repaired.empty() && (repaired.back() == ' ' || repaired.back() == '\t' ||
                                         repaired.back() == '\r' || repaired.back() == '\n')) {
                repaired.pop_back();
            }
            if (!repaired.empty() && repaired.back() == '\\') {
                repaired.pop_back();
            }
            repaired.push_back('"');
        }

        // Trim trailing whitespace
        while (!repaired.empty() && (repaired.back() == ' ' || repaired.back() == '\t' ||
                                     repaired.back() == '\r' || repaired.back() == '\n')) {
            repaired.pop_back();
        }

        if (!repaired.empty() && repaired.back() == ',') {
            repaired.pop_back();
        } else if (!repaired.empty() && repaired.back() == ':') {
            repaired.append(" null");
        }

        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (*it == '{') {
                repaired.push_back('}');
            } else if (*it == '[') {
                repaired.push_back(']');
            }
        }

        if (try_parse_json_lenient(repaired, out_json, out_error)) {
            out_end_pos = text.size();
            return true;
        }
    }

    return false;
}

static bool extract_single_tool_call(const nlohmann::json & j, ToolCall & tc) {
    if (!j.is_object()) return false;
    std::string tool_name;
    if (j.contains("name") && j["name"].is_string()) {
        tool_name = j["name"].get<std::string>();
    } else if (j.contains("tool") && j["tool"].is_string()) {
        tool_name = j["tool"].get<std::string>();
    } else if (j.contains("action") && j["action"].is_string()) {
        tool_name = j["action"].get<std::string>();
    }

    if (tool_name.empty()) return false;

    tc.name = std::move(tool_name);
    if (j.contains("arguments")) {
        if (j["arguments"].is_string()) {
            try {
                tc.arguments = nlohmann::json::parse(j["arguments"].get<std::string>());
            } catch (...) {
                tc.arguments = {{"input", j["arguments"].get<std::string>()}};
            }
        } else if (j["arguments"].is_object() || j["arguments"].is_array()) {
            tc.arguments = j["arguments"];
        } else {
            tc.arguments = nlohmann::json::object();
        }
    } else if (j.contains("parameters") && (j["parameters"].is_object() || j["parameters"].is_array())) {
        tc.arguments = j["parameters"];
    } else if (j.contains("args") && (j["args"].is_object() || j["args"].is_array())) {
        tc.arguments = j["args"];
    } else {
        tc.arguments = nlohmann::json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() != "name" && it.key() != "tool" && it.key() != "action") {
                tc.arguments[it.key()] = it.value();
            }
        }
    }
    return true;
}

static size_t parse_tool_calls_from_text_segment(std::string_view text, std::vector<ToolCall> & tool_calls, std::string * out_error = nullptr) {
    size_t last_end = 0;
    // 1. First check if the text contains a JSON array [...]
    size_t arr_pos = text.find('[');
    size_t obj_pos = text.find('{');

    if (arr_pos != std::string_view::npos && (obj_pos == std::string_view::npos || arr_pos < obj_pos)) {
        nlohmann::json j;
        size_t end_pos = 0;
        if (try_extract_and_parse_json(text, arr_pos, '[', j, end_pos, out_error)) {
            if (j.is_array()) {
                for (const auto & elem : j) {
                    ToolCall tc;
                    if (extract_single_tool_call(elem, tc)) {
                        tool_calls.push_back(std::move(tc));
                    }
                }
                if (!tool_calls.empty()) {
                    if (out_error) out_error->clear();
                    return end_pos;
                }
            }
        }
    }

    // 2. Scan all JSON objects {...} in this segment
    size_t cur = 0;
    while (cur < text.size()) {
        size_t next_obj = text.find('{', cur);
        if (next_obj == std::string_view::npos) break;

        nlohmann::json j;
        size_t end_pos = 0;
        if (try_extract_and_parse_json(text, next_obj, '{', j, end_pos, out_error)) {
            ToolCall tc;
            if (extract_single_tool_call(j, tc)) {
                tool_calls.push_back(std::move(tc));
                last_end = end_pos;
            }
            cur = (end_pos > next_obj) ? end_pos : (next_obj + 1);
        } else {
            cur = next_obj + 1;
        }
    }

    if (!tool_calls.empty() && out_error) {
        out_error->clear();
    }
    return last_end;
}

bool parse_tool_calls(std::string_view response, std::vector<ToolCall> & tool_calls, std::string * out_error) {
    tool_calls.clear();
    if (out_error) {
        out_error->clear();
    }

    struct TagInfo {
        std::string_view open_tag;
        std::string_view close_tag;
    };

    static const TagInfo k_tags[] = {
        {"<tool_call>", "</tool_call>"},
        {"<tool_calls>", "</tool_calls>"},
        {"<tool-call>", "</tool-call>"},
        {"<action>", "</action>"},
        {"[TOOL_CALL]", "[/TOOL_CALL]"},
        {"```tool_call", "```"},
        {"```json", "```"}
    };

    struct TagMatch {
        size_t open_pos;
        size_t content_start;
        size_t content_end;
        size_t end_pos;
        bool has_close_tag;
    };

    std::vector<TagMatch> matches;
    size_t search_pos = 0;

    while (search_pos < response.size()) {
        size_t earliest_pos = std::string_view::npos;
        size_t matched_tag_idx = 0;

        for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); ++i) {
            size_t pos = response.find(k_tags[i].open_tag, search_pos);
            if (pos != std::string_view::npos && (earliest_pos == std::string_view::npos || pos < earliest_pos)) {
                earliest_pos = pos;
                matched_tag_idx = i;
            }
        }

        if (earliest_pos == std::string_view::npos) {
            break;
        }

        const auto & tag = k_tags[matched_tag_idx];
        size_t open_pos = earliest_pos;
        size_t content_start = earliest_pos + tag.open_tag.size();
        size_t content_end = response.size();
        size_t end_pos = response.size();
        bool has_close_tag = false;

        size_t close_pos = response.find(tag.close_tag, content_start);
        if (close_pos != std::string_view::npos) {
            content_end = close_pos;
            end_pos = close_pos + tag.close_tag.size();
            search_pos = end_pos;
            has_close_tag = true;
        } else {
            size_t next_open = std::string_view::npos;
            for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); ++i) {
                size_t p = response.find(k_tags[i].open_tag, content_start);
                if (p != std::string_view::npos && (next_open == std::string_view::npos || p < next_open)) {
                    next_open = p;
                }
            }
            if (next_open != std::string_view::npos) {
                content_end = next_open;
                end_pos = next_open;
                search_pos = next_open;
            } else {
                content_end = response.size();
                end_pos = response.size();
                search_pos = response.size();
            }
        }

        matches.push_back({open_pos, content_start, content_end, end_pos, has_close_tag});
    }

    if (!matches.empty()) {
        const auto & last_match = matches.back();
        std::string_view after_last = response.substr(last_match.end_pos);
        if (after_last.find_first_not_of(" \t\r\n") != std::string_view::npos) {
            // There is non-whitespace text after the last tag: tags are part of the text, not at the end.
            return false;
        }

        size_t start_match_idx = matches.size() - 1;
        while (start_match_idx > 0) {
            size_t prev_idx = start_match_idx - 1;
            size_t prev_end = matches[prev_idx].end_pos;
            size_t curr_open = matches[start_match_idx].open_pos;
            if (curr_open >= prev_end) {
                std::string_view between = response.substr(prev_end, curr_open - prev_end);
                if (between.find_first_not_of(" \t\r\n") == std::string_view::npos) {
                    start_match_idx = prev_idx;
                    continue;
                }
            }
            break;
        }

        for (size_t i = start_match_idx; i < matches.size(); ++i) {
            const auto & match = matches[i];
            std::string_view tag_content = response.substr(match.content_start, match.content_end - match.content_start);
            parse_tool_calls_from_text_segment(tag_content, tool_calls, out_error);
            if (tool_calls.empty() && out_error) {
                if (!match.has_close_tag && tag_content.find('{') == std::string_view::npos && tag_content.find('[') == std::string_view::npos) {
                    out_error->clear();
                } else if (out_error->empty()) {
                    size_t non_ws = tag_content.find_first_not_of(" \t\r\n");
                    if (non_ws != std::string_view::npos) {
                        nlohmann::json dummy;
                        try_parse_json_lenient(tag_content.substr(non_ws), dummy, out_error);
                    }
                }
            }
        }

        if (!tool_calls.empty()) {
            if (out_error) out_error->clear();
            return true;
        }

        return false;
    }

    // Fallback: If no tool tags found, parse raw JSON from response
    std::string stripped_storage;
    std::string_view fallback_view = response;
    if (response.find("<think>") != std::string_view::npos ||
        response.find("<thought>") != std::string_view::npos ||
        response.find("<reasoning>") != std::string_view::npos) {
        stripped_storage = strip_think_tags(response);
        fallback_view = stripped_storage;
    }

    size_t last_end = parse_tool_calls_from_text_segment(fallback_view, tool_calls, nullptr);
    if (!tool_calls.empty()) {
        if (fallback_view.substr(last_end).find_first_not_of(" \t\r\n") != std::string_view::npos) {
            tool_calls.clear();
            return false;
        }
        return true;
    }
    return false;
}

bool parse_tool_call(std::string_view response, std::string & name, nlohmann::json & arguments, std::string * out_error) {
    std::vector<ToolCall> tool_calls;
    if (parse_tool_calls(response, tool_calls, out_error) && !tool_calls.empty()) {
        name = std::move(tool_calls[0].name);
        arguments = std::move(tool_calls[0].arguments);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tool Approval Handling
// -------------------------------------------------------------{
//  "name": "execute_command",
//  "arguments": {
//    "command": "cd /workspaces/llama-onprem-server && for f in src/llm/llm_engine.cpp src/common/agent.cpp src/common/tools.cpp src/common/context.hpp src/common/agent_backend.hpp src/client.cpp src/server.cpp src/fat_client.cpp; do\n  perl -pi -e 's/Color::(RESET|BOLD|DIM|RED|GREEN|YELLOW|BLUE|MAGENTA|CYAN|WHITE|GRAY)\\b/Color::code(Color::\$1)/g' \"$f\"\ndone\necho \"=== remaining bare Color:: usages (should be empty) ===\"\ngrep -rn \"Color::\" src/ | grep -v \"Color::code(\" | grep -v \"override_enabled\" | grep -v \"code(const char\" | grep -v \"is_enabled\"\necho \"=== DONE ===\"\ngrep -rc \"Color::code(\" src/ | grep -v ':0'"
//  }
//}              --------------

ToolApprovalParseResult parse_tool_approval_input(std::string_view raw_input) {
    size_t first = raw_input.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return ToolApprovalParseResult::INVALID;
    }
    size_t last = raw_input.find_last_not_of(" \t\r\n");
    std::string input(raw_input.substr(first, last - first + 1));

    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (input == "ja" || input == "j" || input == "yes" || input == "y" || input == "1") {
        return ToolApprovalParseResult::ALLOW;
    }
    if (input == "nej" || input == "n" || input == "no" || input == "0") {
        return ToolApprovalParseResult::DENY;
    }
    if (input == "alltid ja" || input == "alltid" || input == "alltidja" ||
        input == "always ja" || input == "always yes" || input == "always" ||
        input == "a" || input == "all") {
        return ToolApprovalParseResult::ALWAYS;
    }

    return ToolApprovalParseResult::INVALID;
}

ToolApproval prompt_tool_approval(std::string_view tool_name, const nlohmann::json &tool_args, std::istream &in,
                                  std::ostream &out) {
    while (true) {
        out << Color::BOLD << Color::YELLOW << "⚠️  Agent vill köra verktyg / wants to execute tool: "
                << Color::CYAN << tool_name << Color::RESET << "\n";
        out << "   " << Color::GRAY << "Parametrar / Arguments: " << Color::RESET << tool_args.dump(2) << "\n";
        out << "   " << Color::BOLD << "Godkänn körning? (ja / nej / alltid ja) [j/n/a]: " << Color::RESET;
        out.flush();

        std::string line;
        if (!std::getline(in, line)) {
            out << "\n";
            return ToolApproval::DENY;
        }

        auto res = parse_tool_approval_input(line);
        switch (res) {
            case ToolApprovalParseResult::ALLOW:
                return ToolApproval::ALLOW;
            case ToolApprovalParseResult::DENY:
                return ToolApproval::DENY;
            case ToolApprovalParseResult::ALWAYS:
                return ToolApproval::ALWAYS;
            case ToolApprovalParseResult::INVALID:
                out << Color::RED << "Ogiltigt val. Ange 'j' (ja), 'n' (nej) eller 'a' (alltid ja)." << Color::RESET << "\n";
                break;
        }
    }
}

std::vector<std::string> parse_allowed_tools(std::string_view tools_str) {
    std::vector<std::string> result;
    if (tools_str.empty()) {
        return result;
    }

    size_t start = 0;
    while (start < tools_str.size()) {
        size_t end = tools_str.find(',', start);
        if (end == std::string_view::npos) {
            end = tools_str.size();
        }

        std::string_view token = tools_str.substr(start, end - start);
        size_t first = token.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos) {
            size_t last = token.find_last_not_of(" \t\r\n");
            std::string t(token.substr(first, last - first + 1));
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            result.push_back(std::move(t));
        }

        start = end + 1;
    }
    return result;
}

bool is_tool_allowed(std::string_view tool_name, bool auto_approve, std::span<const std::string> allowed_tools) {
    if (auto_approve) {
        return true;
    }
    std::string lower_tool(tool_name);
    std::transform(lower_tool.begin(), lower_tool.end(), lower_tool.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (const auto & allowed : allowed_tools) {
        if (allowed == "*" || allowed == "all" || allowed == lower_tool) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Think Tag Removal & Cleaning
// ---------------------------------------------------------------------------

std::string strip_think_tags(std::string_view text) {
    if (text.empty()) return "";

    std::string result(text);

    // 1. Remove self-closing think tags (<think/>, <thought/>, <reasoning/>)
    static const std::regex self_closing_regex(R"(<(think|thought|reasoning)\s*/\s*>)", std::regex::icase);
    result = std::regex_replace(result, self_closing_regex, "");

    // 2. Remove complete think blocks (<think>...</think>, <thought>...</thought>, etc.)
    static const std::regex think_block_regex(R"(<(think|thought|reasoning)>[\s\S]*?</\1>)", std::regex::icase);
    result = std::regex_replace(result, think_block_regex, "");

    // 3. Remove any stray closing tags at the beginning of string
    static const std::regex stray_close_start_regex(R"(^[\s\S]*?</(think|thought|reasoning)>)", std::regex::icase);
    result = std::regex_replace(result, stray_close_start_regex, "");

    // 4. Remove unclosed think tags extending to the end
    static const std::regex unclosed_think_regex(R"(<(think|thought|reasoning)>[\s\S]*$)", std::regex::icase);
    result = std::regex_replace(result, unclosed_think_regex, "");

    // 5. Remove any remaining stray single tags
    static const std::regex stray_any_regex(R"(</?(think|thought|reasoning)\s*>)", std::regex::icase);
    result = std::regex_replace(result, stray_any_regex, "");

    // 6. Trim leading and trailing whitespace
    size_t first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// Thinking Stream Filter Implementation
// ---------------------------------------------------------------------------

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static bool check_open_tag(std::string_view text, size_t pos, size_t & tag_len, bool & self_closing) {
    if (pos >= text.size() || text[pos] != '<') return false;
    static const char * const tag_names[] = {"think", "thought", "reasoning"};
    for (const char * tag_name : tag_names) {
        size_t tlen = std::strlen(tag_name);
        if (pos + 1 + tlen <= text.size()) {
            std::string_view sub = text.substr(pos + 1, tlen);
            if (iequals(sub, tag_name)) {
                size_t p = pos + 1 + tlen;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) {
                    ++p;
                }
                if (p < text.size() && text[p] == '/') {
                    ++p;
                    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) {
                        ++p;
                    }
                    if (p < text.size() && text[p] == '>') {
                        tag_len = (p + 1) - pos;
                        self_closing = true;
                        return true;
                    }
                } else if (p < text.size() && text[p] == '>') {
                    tag_len = (p + 1) - pos;
                    self_closing = false;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool check_close_tag(std::string_view text, size_t pos, size_t & tag_len) {
    if (pos + 1 >= text.size() || text[pos] != '<' || text[pos + 1] != '/') return false;
    static const char * const tag_names[] = {"think", "thought", "reasoning"};
    for (const char * tag_name : tag_names) {
        size_t tlen = std::strlen(tag_name);
        if (pos + 2 + tlen <= text.size()) {
            std::string_view sub = text.substr(pos + 2, tlen);
            if (iequals(sub, tag_name)) {
                size_t p = pos + 2 + tlen;
                while (p < text.size() && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n')) {
                    ++p;
                }
                if (p < text.size() && text[p] == '>') {
                    tag_len = (p + 1) - pos;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool is_partial_tag_at_end(std::string_view text) {
    if (text.empty()) return false;
    size_t last_lt = text.rfind('<');
    if (last_lt == std::string_view::npos) return false;
    std::string_view suffix = text.substr(last_lt);
    if (suffix.find('>') != std::string_view::npos) return false;
    if (suffix.size() > 20) return false;
    return true;
}

ThinkingStreamFilter::ThinkingStreamFilter(OutputCallback cb) : cb_(std::move(cb)) {}

void ThinkingStreamFilter::emit_normal(std::string_view piece) {
    if (!piece.empty() && cb_) {
        cb_(piece, false);
    }
}

void ThinkingStreamFilter::emit_thinking(std::string_view piece) {
    if (!piece.empty() && cb_) {
        cb_(piece, true);
    }
}

void ThinkingStreamFilter::process(std::string_view piece) {
    buffer_.append(piece);
    process_internal();
}

void ThinkingStreamFilter::process_internal() {
    while (!buffer_.empty()) {
        if (state_ == State::NORMAL) {
            size_t open_pos = std::string::npos;
            size_t open_len = 0;
            bool self_closing = false;

            size_t close_pos = std::string::npos;
            size_t close_len = 0;

            for (size_t i = 0; i < buffer_.size(); ++i) {
                if (buffer_[i] == '<') {
                    if (check_open_tag(buffer_, i, open_len, self_closing)) {
                        open_pos = i;
                        break;
                    }
                    if (check_close_tag(buffer_, i, close_len)) {
                        close_pos = i;
                        break;
                    }
                }
            }

            if (open_pos != std::string::npos) {
                if (open_pos > 0) {
                    emit_normal(std::string_view(buffer_).substr(0, open_pos));
                }
                if (self_closing) {
                    buffer_.erase(0, open_pos + open_len);
                    continue;
                } else {
                    buffer_.erase(0, open_pos + open_len);
                    state_ = State::BUFFERING_THINKING;
                    continue;
                }
            } else if (close_pos != std::string::npos) {
                // Orphaned close tag: discard preceding text if at start, or drop tag
                if (close_pos > 0) {
                    std::string_view prec = std::string_view(buffer_).substr(0, close_pos);
                    size_t non_ws = prec.find_first_not_of(" \t\r\n");
                    if (non_ws != std::string_view::npos) {
                        emit_normal(prec);
                    }
                }
                buffer_.erase(0, close_pos + close_len);
                continue;
            } else {
                if (is_partial_tag_at_end(buffer_)) {
                    size_t last_lt = buffer_.rfind('<');
                    if (last_lt > 0) {
                        emit_normal(std::string_view(buffer_).substr(0, last_lt));
                        buffer_.erase(0, last_lt);
                    }
                    return;
                } else {
                    emit_normal(buffer_);
                    buffer_.clear();
                    return;
                }
            }
        } else if (state_ == State::BUFFERING_THINKING) {
            size_t close_pos = std::string::npos;
            size_t close_len = 0;
            for (size_t i = 0; i < buffer_.size(); ++i) {
                if (buffer_[i] == '<' && check_close_tag(buffer_, i, close_len)) {
                    close_pos = i;
                    break;
                }
            }

            if (close_pos != std::string::npos) {
                std::string_view thought = std::string_view(buffer_).substr(0, close_pos);
                size_t non_ws = thought.find_first_not_of(" \t\r\n");
                if (non_ws != std::string_view::npos) {
                    emit_thinking("💭 ");
                    size_t last_ws = thought.find_last_not_of(" \t\r\n");
                    emit_thinking(thought.substr(non_ws, last_ws - non_ws + 1));
                    emit_thinking("\n");
                }
                buffer_.erase(0, close_pos + close_len);
                state_ = State::NORMAL;
                continue;
            } else {
                size_t non_ws_count = 0;
                for (char c : buffer_) {
                    if (!std::isspace(static_cast<unsigned char>(c))) {
                        ++non_ws_count;
                    }
                }
                if (non_ws_count >= 40) {
                    state_ = State::STREAMING_THINKING;
                    emit_thinking("💭 ");
                    if (is_partial_tag_at_end(buffer_)) {
                        size_t last_lt = buffer_.rfind('<');
                        if (last_lt > 0) {
                            emit_thinking(std::string_view(buffer_).substr(0, last_lt));
                            buffer_.erase(0, last_lt);
                        }
                    } else {
                        emit_thinking(buffer_);
                        buffer_.clear();
                    }
                }
                return;
            }
        } else if (state_ == State::STREAMING_THINKING) {
            size_t close_pos = std::string::npos;
            size_t close_len = 0;
            for (size_t i = 0; i < buffer_.size(); ++i) {
                if (buffer_[i] == '<' && check_close_tag(buffer_, i, close_len)) {
                    close_pos = i;
                    break;
                }
            }

            if (close_pos != std::string::npos) {
                if (close_pos > 0) {
                    emit_thinking(std::string_view(buffer_).substr(0, close_pos));
                }
                emit_thinking("\n");
                buffer_.erase(0, close_pos + close_len);
                state_ = State::NORMAL;
                continue;
            } else {
                if (is_partial_tag_at_end(buffer_)) {
                    size_t last_lt = buffer_.rfind('<');
                    if (last_lt > 0) {
                        emit_thinking(std::string_view(buffer_).substr(0, last_lt));
                        buffer_.erase(0, last_lt);
                    }
                    return;
                } else {
                    emit_thinking(buffer_);
                    buffer_.clear();
                    return;
                }
            }
        }
    }
}

void ThinkingStreamFilter::flush() {
    if (state_ == State::BUFFERING_THINKING) {
        std::string_view thought = buffer_;
        size_t non_ws = thought.find_first_not_of(" \t\r\n");
        if (non_ws != std::string_view::npos) {
            emit_thinking("💭 ");
            size_t last_ws = thought.find_last_not_of(" \t\r\n");
            emit_thinking(thought.substr(non_ws, last_ws - non_ws + 1));
            emit_thinking("\n");
        }
        buffer_.clear();
        state_ = State::NORMAL;
    } else if (state_ == State::STREAMING_THINKING) {
        if (!buffer_.empty()) {
            emit_thinking(buffer_);
            emit_thinking("\n");
            buffer_.clear();
        }
        state_ = State::NORMAL;
    } else if (state_ == State::NORMAL) {
        if (!buffer_.empty()) {
            std::string cleaned = strip_think_tags(buffer_);
            if (!cleaned.empty()) {
                emit_normal(cleaned);
            }
            buffer_.clear();
        }
    }
}
