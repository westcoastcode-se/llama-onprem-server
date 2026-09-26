#include "common/agent.hpp"
#include "common/color.hpp"
#include "common/context.hpp"
#include "common/tools.hpp"
#include "common/utf8.hpp"
#include <algorithm>
#include <cstdio>
#include <format>
#include <iostream>
#include <print>

void print_slash_commands_help()
{
    std::print("\n{}Available Agent Commands:{}\n"
               "  {}/exec <cmd>, /sh <cmd>{} - Run a shell command directly and print result to stdout\n"
               "  {}/tools{}           - List all available tools and their parameter schemas\n"
               "  {}/subagents{}       - Toggle sub-agent tool delegation (enable/disable)\n"
               "  {}/approval{}        - Toggle tool approval mode (Require approval / Auto-approve)\n"
               "  {}/context{}         - Show current context usage and memory statistics\n"
               "  {}/compact{}         - Manually compact conversation context history\n"
               "  {}/clear, /reset{}   - Clear conversation history and reset context memory\n"
               "  {}/system{}          - Show the active agent system prompt\n"
               "  {}/exit, /quit{}     - Terminate session\n\n",
               Color::BOLD, Color::RESET, Color::CYAN, Color::RESET, Color::CYAN, Color::RESET, Color::CYAN,
               Color::RESET, Color::CYAN, Color::RESET, Color::CYAN, Color::RESET, Color::CYAN, Color::RESET,
               Color::CYAN, Color::RESET, Color::CYAN, Color::RESET, Color::CYAN, Color::RESET);
}

bool compact_context(IAgentBackend & backend,
                    std::vector<Protocol::ChatMessage> & messages,
                    bool quiet) {
    if (messages.size() <= 2) {
        if (!quiet) {
            std::println("{}[agent] Conversation history is already minimal (no older messages to compact).{}\n",
                         Color::YELLOW, Color::RESET);
        }
        return false;
    }

    if (!quiet) {
        std::println("{}[agent] Compacting conversation context...{}", Color::CYAN, Color::RESET);
    }

    size_t keep_recent = 2;
    size_t start_recent = messages.size() > keep_recent + 1 ? messages.size() - keep_recent : 1;
    std::span<const Protocol::ChatMessage> to_summarize(messages.data() + 1, start_recent - 1);

    std::string summary_prompt = Context::build_summary_prompt(to_summarize);
    std::string summary = backend.generate(summary_prompt, nullptr, 0.3f);
    summary = strip_think_tags(summary);

    size_t first = summary.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        size_t last = summary.find_last_not_of(" \t\r\n");
        summary = summary.substr(first, last - first + 1);
    }

    if (summary.empty()) {
        summary = Context::create_fallback_summary(to_summarize);
    }

    size_t prev_count = messages.size();
    Context::compact_messages(messages, summary, keep_recent);

    backend.reset_context();

    if (!quiet) {
        std::println("{}[agent] Context compacted successfully: reduced from {} to {} messages.{}\n", Color::GREEN,
                     prev_count, messages.size(), Color::RESET);
    }
    return true;
}

std::string run_subagent(IAgentBackend & backend,
                         std::string_view task,
                         std::span<const Tool> base_tools,
                         std::string_view custom_system_prompt,
                         float temperature,
                         int max_iterations,
                         bool & auto_approve,
                         std::span<const std::string> allowed_tools,
                         bool quiet) {
    if (!quiet) {
        std::println("{}\n🤖 [sub-agent started] Task: {}{}", Color::CYAN, task, Color::RESET);
    }

    const std::string focused =
        "You are a focused sub-agent tasked with solving a specific sub-task. Use your tools efficiently, execute any "
        "necessary tools to accomplish the task, and provide a comprehensive final answer.";
    const std::string sub_custom_prompt =
        custom_system_prompt.empty() ? focused : std::format("{}\n{}", custom_system_prompt, focused);

    const std::string sub_system_prompt = build_system_prompt(base_tools, sub_custom_prompt);
    std::vector<Protocol::ChatMessage> sub_messages;
    sub_messages.push_back({"system", sub_system_prompt});
    sub_messages.push_back({"user", std::string(task)});

    std::string last_tool_call_signature;
    int duplicate_tool_count = 0;
    std::string final_answer;

    for (int iter = 0; iter < max_iterations; ++iter) {
        ThinkingStreamFilter stream_filter([quiet](std::string_view piece, bool is_thinking) {
            if (!quiet) {
                if (is_thinking)
                {
                    std::print("{}{}{}", Color::DIM, piece, Color::RESET);
                }
                else
                {
                    std::print("{}{}", Color::DIM, piece);
                }
                fflush(stdout);
            }
        });

        std::string response = backend.chat(sub_messages, [&stream_filter](std::string_view piece) -> bool {
            stream_filter.process(piece);
            return true;
        }, temperature);
        stream_filter.flush();
        if (!quiet) {
            std::println("{}", Color::RESET);
        }

        if (response.empty()) {
            break;
        }

        std::string clean_hist = strip_think_tags(response);
        sub_messages.push_back({"assistant", clean_hist.empty() ? response : clean_hist});

        std::vector<ToolCall> tool_calls;
        std::string parse_error;
        if (parse_tool_calls(response, tool_calls, &parse_error) && !tool_calls.empty()) {
            std::string call_signature;
            for (const auto & tc : tool_calls) {
                call_signature += tc.name + ":" + tc.arguments.dump() + ";";
            }

            if (call_signature == last_tool_call_signature) {
                duplicate_tool_count++;
            } else {
                last_tool_call_signature = call_signature;
                duplicate_tool_count = 0;
            }

            if (duplicate_tool_count >= 3) {
                if (!quiet) {
                    std::println("{}  [sub-agent loop detected: skipping duplicate tool call]{}", Color::RED, Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated identical tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                sub_messages.push_back({"tool", loop_warning});
                continue;
            }

            bool stop_execution = false;
            for (size_t idx = 0; idx < tool_calls.size(); ++idx) {
                const auto & tc = tool_calls[idx];
                if (!is_tool_allowed(tc.name, auto_approve, allowed_tools)) {
                    ToolApproval approval = prompt_tool_approval(tc.name, tc.arguments);
                    if (approval == ToolApproval::ALWAYS) {
                        auto_approve = true;
                        if (!quiet) {
                            std::println("{}[sub-agent] Alltid godkänn aktiverat: efterföljande verktygsanrop tillåts automatiskt.{}",
                                         Color::GREEN, Color::RESET);
                        }
                    } else if (approval == ToolApproval::DENY) {
                        if (!quiet) {
                            std::println("{}❌ [sub-agent verktygskörning nekades av användaren / Tool execution denied by user: {}]{}",
                                         Color::RED, tc.name, Color::RESET);
                        }
                        const std::string denial_msg = std::format(
                            "<tool_response>\nerror: tool execution was denied by the user for tool '{}'.\n</tool_response>",
                            tc.name);
                        sub_messages.push_back({"tool", denial_msg});
                        stop_execution = true;
                        break;
                    }
                }

                if (!quiet) {
                    if (tool_calls.size() > 1) {
                        std::println("{}  ⚙️ [sub-agent action ({}/{}): {}{}{}]{}", Color::CYAN, idx + 1, tool_calls.size(),
                                     Color::BOLD, tc.name, Color::CYAN, Color::RESET);
                    } else {
                        std::println("{}  ⚙️ [sub-agent action: {}{}{}]{}", Color::CYAN, Color::BOLD, tc.name, Color::CYAN,
                                     Color::RESET);
                    }
                    std::println("{}     Args: {}{}", Color::GRAY, tc.arguments.dump(2), Color::RESET);
                }

                const std::string tool_result = run_tool(base_tools, tc.name, tc.arguments);

                if (!quiet) {
                    std::string preview = tool_result;
                    if (preview.size() > 140) {
                        preview = preview.substr(0, 140) + "...";
                    }
                    std::ranges::replace(preview, '\n', ' ');
                    std::println("{}  📋 [sub-agent observation ({} chars): {}]{}", Color::MAGENTA, tool_result.size(),
                                 preview, Color::RESET);
                }

                const std::string tool_message = std::format("<tool_response>\n{}\n</tool_response>", tool_result);
                sub_messages.push_back({"tool", tool_message});
            }

            if (stop_execution) {
                continue;
            }
            continue;
        } else if (!parse_error.empty()) {
            std::string call_signature = "parse_error:" + parse_error;
            if (call_signature == last_tool_call_signature) {
                duplicate_tool_count++;
            } else {
                last_tool_call_signature = call_signature;
                duplicate_tool_count = 0;
            }

            if (duplicate_tool_count >= 3) {
                if (!quiet) {
                    std::println("{}  [sub-agent loop detected: skipping duplicate invalid tool call]{}", Color::RED,
                                 Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated invalid tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                sub_messages.push_back({"tool", loop_warning});
                continue;
            }

            if (!quiet) {
                std::println("{}❌ [sub-agent felaktig JSON i verktygsanrop / Invalid JSON in tool call: {}]{}", Color::RED,
                             parse_error, Color::RESET);
            }
            const std::string tool_message = std::format("<tool_response>\nerror: {}\n</tool_response>", parse_error);
            sub_messages.push_back({"tool", tool_message});
            continue;
        }

        final_answer = strip_think_tags(response);
        if (!quiet) {
            std::println("{}🤖 [sub-agent completed task successfully]{}", Color::GREEN, Color::RESET);
        }
        return final_answer;
    }

    if (!quiet) {
        std::println("{}🤖 [sub-agent iteration limit reached]{}", Color::YELLOW, Color::RESET);
    }
    if (!sub_messages.empty() && sub_messages.back().role == "assistant") {
        return strip_think_tags(sub_messages.back().content);
    }
    return final_answer.empty() ? "sub-agent completed max iterations without explicit final answer" : strip_think_tags(final_answer);
}

bool execute_agent_turn(IAgentBackend & backend,
                        std::vector<Protocol::ChatMessage> & messages,
                        std::span<const Tool> tools,
                        std::string_view user_input,
                        float temperature,
                        int max_iterations,
                        bool & auto_approve,
                        std::span<const std::string> allowed_tools,
                        std::string * out_response,
                        bool quiet) {
    if (user_input.starts_with("/exec ") || user_input.starts_with("/sh ") ||
        user_input.starts_with("/run ") || user_input.starts_with("/cmd ")) {
        size_t space_pos = user_input.find(' ');
        std::string_view cmd_view = user_input.substr(space_pos + 1);
        size_t first_non = cmd_view.find_first_not_of(" \t\r\n");
        if (first_non != std::string_view::npos) {
            std::string cmd(cmd_view.substr(first_non));
            nlohmann::json args = {{"command", cmd}};
            std::string res = run_tool(tools, "execute_command", args);
            std::println("{}", res);
            if (out_response) *out_response = res;
        }
        return true;
    }

    // Auto-compact before new turn if context is full
    {
        int used_ctx = backend.get_used_context();
        int n_ctx = backend.get_context_size();
        if (Context::should_compact(used_ctx, n_ctx) && messages.size() > 2) {
            if (!quiet) {
                float pct = Context::get_usage_percentage(used_ctx, n_ctx);
                std::println("{}[agent] Context usage high ({} / {} tokens, {:.1f}%). Automatically compacting context...{}",
                             Color::YELLOW, used_ctx, n_ctx, pct, Color::RESET);
            }
            compact_context(backend, messages, quiet);
        }
    }

    // Add user turn to conversation history
    messages.push_back({"user", std::string(user_input)});

    std::string last_tool_call_signature;
    int duplicate_tool_count = 0;
    std::string final_turn_response;

    // Autonomous ReAct loop
    for (int iter = 0; iter < max_iterations; ++iter) {
        // Auto-compact within ReAct loop if context became too large
        {
            int used_ctx = backend.get_used_context();
            int n_ctx = backend.get_context_size();
            if (Context::should_compact(used_ctx, n_ctx) && messages.size() > 2) {
                if (!quiet) {
                    float pct = Context::get_usage_percentage(used_ctx, n_ctx);
                    std::println(
                        "{}[agent] Context usage high ({} / {} tokens, {:.1f}%). Automatically compacting context...{}",
                        Color::YELLOW, used_ctx, n_ctx, pct, Color::RESET);
                }
                compact_context(backend, messages, quiet);
            }
        }

        ThinkingStreamFilter stream_filter([quiet](std::string_view piece, bool is_thinking) {
            if (!quiet) {
                if (is_thinking)
                {
                    std::print("{}{}{}", Color::DIM, piece, Color::RESET);
                }
                else
                {
                    std::print("{}{}", Color::YELLOW, piece);
                }
                fflush(stdout);
            }
        });

        std::string response = backend.chat(messages, [&stream_filter](std::string_view piece) -> bool {
            stream_filter.process(piece);
            return true;
        }, temperature);
        stream_filter.flush();
        if (!quiet) {
            std::println("{}", Color::RESET);
        }

        if (response.empty()) {
            break;
        }

        std::string clean_hist = strip_think_tags(response);
        messages.push_back({"assistant", clean_hist.empty() ? response : clean_hist});
        final_turn_response = response;

        // Check if assistant called one or more tools
        std::vector<ToolCall> tool_calls;
        std::string parse_error;
        if (parse_tool_calls(response, tool_calls, &parse_error) && !tool_calls.empty()) {
            std::string call_signature;
            for (const auto & tc : tool_calls) {
                call_signature += tc.name + ":" + tc.arguments.dump() + ";";
            }

            if (call_signature == last_tool_call_signature) {
                duplicate_tool_count++;
            } else {
                last_tool_call_signature = call_signature;
                duplicate_tool_count = 0;
            }

            if (duplicate_tool_count >= 3) {
                if (!quiet) {
                    std::println("{}[agent loop detected: skipping duplicate tool call]{}", Color::RED, Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated identical tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                messages.push_back({"tool", loop_warning});
                continue;
            }

            bool stop_execution = false;
            for (size_t idx = 0; idx < tool_calls.size(); ++idx) {
                const auto & tc = tool_calls[idx];
                if (!is_tool_allowed(tc.name, auto_approve, allowed_tools)) {
                    ToolApproval approval = prompt_tool_approval(tc.name, tc.arguments);
                    if (approval == ToolApproval::ALWAYS) {
                        auto_approve = true;
                        if (!quiet) {
                            std::println("{}[agent] Alltid godkänn aktiverat: efterföljande verktygsanrop tillåts automatiskt.{}",
                                         Color::GREEN, Color::RESET);
                        }
                    } else if (approval == ToolApproval::DENY) {
                        if (!quiet) {
                            std::println("{}❌ [Verktygskörning nekades av användaren / Tool execution denied by user: {}]{}",
                                         Color::RED, tc.name, Color::RESET);
                        }
                        const std::string denial_msg = std::format(
                            "<tool_response>\nerror: tool execution was denied by the user for tool '{}'.\n</tool_response>",
                            tc.name);
                        messages.push_back({"tool", denial_msg});
                        stop_execution = true;
                        break;
                    }
                }

                if (!quiet) {
                    if (tool_calls.size() > 1) {
                        std::println("{}⚙️  [Agent Action ({}/{}): {}{}{}]{}", Color::CYAN, idx + 1, tool_calls.size(),
                                     Color::BOLD, tc.name, Color::CYAN, Color::RESET);
                    } else {
                        std::println("{}⚙️  [Agent Action: {}{}{}]{}", Color::CYAN, Color::BOLD, tc.name, Color::CYAN,
                                     Color::RESET);
                    }
                    std::println("{}   Args: {}{}", Color::GRAY, tc.arguments.dump(2), Color::RESET);
                }

                const std::string tool_result = run_tool(tools, tc.name, tc.arguments);

                if (!quiet) {
                    std::string preview = tool_result;
                    if (preview.size() > 180) {
                        preview = preview.substr(0, 180) + "...";
                    }
                    std::ranges::replace(preview, '\n', ' ');
                    std::println("{}📋 [Observation ({} chars): {}]{}", Color::MAGENTA, tool_result.size(), preview,
                                 Color::RESET);
                }

                const std::string tool_message = std::format("<tool_response>\n{}\n</tool_response>", tool_result);
                messages.push_back({"tool", tool_message});
            }

            if (stop_execution) {
                continue;
            }
            continue;
        } else if (!parse_error.empty()) {
            std::string call_signature = "parse_error:" + parse_error;
            if (call_signature == last_tool_call_signature) {
                duplicate_tool_count++;
            } else {
                last_tool_call_signature = call_signature;
                duplicate_tool_count = 0;
            }

            if (duplicate_tool_count >= 3) {
                if (!quiet) {
                    std::println("{}[agent loop detected: skipping duplicate invalid tool call]{}", Color::RED, Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated invalid tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                messages.push_back({"tool", loop_warning});
                continue;
            }

            if (!quiet) {
                std::println("{}❌ [Felaktig JSON i verktygsanrop / Invalid JSON in tool call: {}]{}", Color::RED,
                             parse_error, Color::RESET);
            }
            const std::string tool_message = std::format("<tool_response>\nerror: {}\n</tool_response>", parse_error);
            messages.push_back({"tool", tool_message});
            continue;
        }

        // No tool call -> final answer reached
        break;
    }

    if (quiet) {
        std::string cleaned = strip_think_tags(final_turn_response);
        std::println("{}", cleaned);
    } else {
        int used_ctx = backend.get_used_context();
        int n_ctx = backend.get_context_size();
        if (n_ctx > 0 && used_ctx > 0) {
            float pct = Context::get_usage_percentage(used_ctx, n_ctx);
            std::println("{}📊 [Context: {} / {} tokens ({:.1f}%)]{}", Color::DIM, used_ctx, n_ctx, pct, Color::RESET);
        }
        std::println("");
    }

    if (out_response) *out_response = strip_think_tags(final_turn_response);
    return true;
}

int run_agent_session(IAgentBackend & backend,
                      AgentSessionConfig & config,
                      std::string_view mode_title,
                      std::string_view extra_info) {
    const auto base_tools = get_base_tools();
    auto subagent_runner = [&](std::string_view task) -> std::string {
        return run_subagent(backend, task, base_tools, config.custom_system_prompt,
                            config.temperature, config.max_iterations, config.auto_approve,
                            config.allowed_tools, config.quiet);
    };

    auto tools = get_registered_tools(config.enable_subagents, subagent_runner);
    std::string system_prompt = build_system_prompt(tools, config.custom_system_prompt);

    std::vector<Protocol::ChatMessage> messages;
    messages.push_back({"system", system_prompt});

    // If single command mode is specified, execute once and exit
    if (!config.single_command.empty()) {
        bool ok = execute_agent_turn(backend, messages, tools, config.single_command,
                                     config.temperature, config.max_iterations, config.auto_approve,
                                     config.allowed_tools, nullptr, config.quiet);
        return ok ? 0 : 1;
    }

    std::println("{}======================================================={}", Color::CYAN, Color::RESET);
    std::println("{}        {}         {}", Color::BOLD, mode_title, Color::RESET);
    if (!extra_info.empty()) {
        std::println("  {}", extra_info);
    }
    std::println("  Tools registered: {} | Sub-agents: {} | Approval: {} | Context: {} | Temp: {:.2f}", tools.size(),
                 config.enable_subagents ? "ENABLED" : "DISABLED",
                 config.auto_approve ? "AUTO-APPROVE" : "REQUIRE APPROVAL", backend.get_context_size(),
                 config.temperature);
    std::println("  Type {}/help{} for commands, {}/tools{} to list available tools", Color::GREEN, Color::RESET,
                 Color::GREEN, Color::RESET);
    std::println("{}======================================================={}\n", Color::CYAN, Color::RESET);

    while (true) {
        std::print("{}{}agent> {}", Color::BOLD, Color::GREEN, Color::RESET);
        std::string user_input;
        if (!std::getline(std::cin, user_input)) {
            break;
        }

        // Trim leading and trailing whitespace
        size_t first = user_input.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        size_t last = user_input.find_last_not_of(" \t\r\n");
        user_input = user_input.substr(first, last - first + 1);

        if (user_input.empty()) {
            continue;
        }

        // Handle slash commands
        if (user_input == "/exit" || user_input == "/quit") {
            std::println("{}Exiting agent session.{}", Color::YELLOW, Color::RESET);
            break;
        }

        if (user_input == "/help") {
            print_slash_commands_help();
            continue;
        }

        if (user_input == "/subagents" || user_input == "/subagent" || user_input == "/sub-agents") {
            config.enable_subagents = !config.enable_subagents;
            tools = get_registered_tools(config.enable_subagents, subagent_runner);
            system_prompt = build_system_prompt(tools, config.custom_system_prompt);
            if (!messages.empty()) {
                messages[0] = {"system", system_prompt};
            }
            std::println("{}[agent] Sub-agents are now {}.{}\n", Color::CYAN,
                         config.enable_subagents ? "ENABLED" : "DISABLED", Color::RESET);
            continue;
        }

        if (user_input == "/approval" || user_input == "/approve" || user_input == "/confirm") {
            config.auto_approve = !config.auto_approve;
            std::println("{}[agent] Tool approval mode: {}{}\n", Color::CYAN,
                         config.auto_approve ? "AUTO-APPROVE (always allow)"
                                             : "REQUIRE APPROVAL (prompt ja/nej/alltid ja)",
                         Color::RESET);
            continue;
        }

        if (user_input == "/context") {
            backend.refresh_context_info();
            Context::print_context_info(backend.get_used_context(), backend.get_context_size(), messages);
            continue;
        }

        if (user_input == "/compact") {
            compact_context(backend, messages, false);
            continue;
        }

        if (user_input == "/tools") {
            std::println("\n{}Registered Agent Tools:{}", Color::BOLD, Color::RESET);
            for (const auto & tool : tools) {
                std::println("\n{}• {}{}", Color::CYAN, tool.name, Color::RESET);
                std::println("  {}Description:{} {}", Color::DIM, Color::RESET, tool.description);
                std::println("  {}Schema:{}\n    {}", Color::DIM, Color::RESET, tool.schema_doc);
            }
            std::println("");
            continue;
        }

        if (user_input == "/system") {
            std::println("\n{}Current System Prompt:{}\n{}{}{}\n", Color::BOLD, Color::RESET, Color::GRAY, system_prompt,
                         Color::RESET);
            continue;
        }

        if (user_input == "/clear" || user_input == "/reset") {
            messages.clear();
            messages.push_back({"system", system_prompt});
            backend.reset_context();
            std::println("{}[agent] Conversation history and context memory reset.{}\n", Color::GREEN, Color::RESET);
            continue;
        }

        execute_agent_turn(backend, messages, tools, user_input,
                           config.temperature, config.max_iterations, config.auto_approve,
                           config.allowed_tools, nullptr, config.quiet);
    }

    return 0;
}
