#include "common/agent.hpp"
#include "common/color.hpp"
#include "common/context.hpp"
#include "common/tools.hpp"
#include "common/utf8.hpp"
#include <algorithm>
#include <cstdio>
#include <iostream>

void print_slash_commands_help() {
    printf("\n%sAvailable Agent Commands:%s\n", Color::BOLD, Color::RESET);
    printf("  %s/exec <cmd>, /sh <cmd>%s - Run a shell command directly and print result to stdout\n", Color::CYAN, Color::RESET);
    printf("  %s/tools%s           - List all available tools and their parameter schemas\n", Color::CYAN, Color::RESET);
    printf("  %s/subagents%s       - Toggle sub-agent tool delegation (enable/disable)\n", Color::CYAN, Color::RESET);
    printf("  %s/approval%s        - Toggle tool approval mode (Require approval / Auto-approve)\n", Color::CYAN, Color::RESET);
    printf("  %s/context%s         - Show current context usage and memory statistics\n", Color::CYAN, Color::RESET);
    printf("  %s/compact%s         - Manually compact conversation context history\n", Color::CYAN, Color::RESET);
    printf("  %s/clear, /reset%s   - Clear conversation history and reset context memory\n", Color::CYAN, Color::RESET);
    printf("  %s/system%s          - Show the active agent system prompt\n", Color::CYAN, Color::RESET);
    printf("  %s/exit, /quit%s     - Terminate session\n\n", Color::CYAN, Color::RESET);
}

bool compact_context(IAgentBackend & backend,
                    std::vector<Protocol::ChatMessage> & messages,
                    bool quiet) {
    if (messages.size() <= 2) {
        if (!quiet) {
            printf("%s[agent] Conversation history is already minimal (no older messages to compact).%s\n\n",
                   Color::YELLOW, Color::RESET);
        }
        return false;
    }

    if (!quiet) {
        printf("%s[agent] Compacting conversation context...%s\n", Color::CYAN, Color::RESET);
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
        printf("%s[agent] Context compacted successfully: reduced from %zu to %zu messages.%s\n\n",
               Color::GREEN, prev_count, messages.size(), Color::RESET);
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
        printf("%s\n🤖 [sub-agent started] Task: %.*s%s\n", Color::CYAN, static_cast<int>(task.size()), task.data(), Color::RESET);
    }

    std::string sub_custom_prompt = custom_system_prompt.empty()
        ? "You are a focused sub-agent tasked with solving a specific sub-task. Use your tools efficiently, execute any necessary tools to accomplish the task, and provide a comprehensive final answer."
        : std::string(custom_system_prompt) + "\nYou are a focused sub-agent tasked with solving a specific sub-task. Use your tools efficiently, execute any necessary tools to accomplish the task, and provide a comprehensive final answer.";

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
                if (is_thinking) {
                    printf("%s%.*s%s", Color::DIM, static_cast<int>(piece.size()), piece.data(), Color::RESET);
                } else {
                    printf("%s%.*s", Color::DIM, static_cast<int>(piece.size()), piece.data());
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
            printf("%s\n", Color::RESET);
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
                    printf("%s  [sub-agent loop detected: skipping duplicate tool call]%s\n", Color::RED, Color::RESET);
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
                            printf("%s[sub-agent] Alltid godkänn aktiverat: efterföljande verktygsanrop tillåts automatiskt.%s\n",
                                   Color::GREEN, Color::RESET);
                        }
                    } else if (approval == ToolApproval::DENY) {
                        if (!quiet) {
                            printf("%s❌ [sub-agent verktygskörning nekades av användaren / Tool execution denied by user: %s]%s\n",
                                   Color::RED, tc.name.c_str(), Color::RESET);
                        }
                        std::string denial_msg = "<tool_response>\nerror: tool execution was denied by the user for tool '" + tc.name + "'.\n</tool_response>";
                        sub_messages.push_back({"tool", denial_msg});
                        stop_execution = true;
                        break;
                    }
                }

                if (!quiet) {
                    if (tool_calls.size() > 1) {
                        printf("%s  ⚙️ [sub-agent action (%zu/%zu): %s%s%s]%s\n",
                               Color::CYAN, idx + 1, tool_calls.size(), Color::BOLD, tc.name.c_str(), Color::CYAN, Color::RESET);
                    } else {
                        printf("%s  ⚙️ [sub-agent action: %s%s%s]%s\n",
                               Color::CYAN, Color::BOLD, tc.name.c_str(), Color::CYAN, Color::RESET);
                    }
                    printf("%s     Args: %s%s\n", Color::GRAY, tc.arguments.dump(2).c_str(), Color::RESET);
                }

                const std::string tool_result = run_tool(base_tools, tc.name, tc.arguments);

                if (!quiet) {
                    std::string preview = tool_result;
                    if (preview.size() > 140) {
                        preview = preview.substr(0, 140) + "...";
                    }
                    std::replace(preview.begin(), preview.end(), '\n', ' ');
                    printf("%s  📋 [sub-agent observation (%zu chars): %s]%s\n",
                           Color::MAGENTA, tool_result.size(), preview.c_str(), Color::RESET);
                }

                const std::string tool_message = "<tool_response>\n" + tool_result + "\n</tool_response>";
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
                    printf("%s  [sub-agent loop detected: skipping duplicate invalid tool call]%s\n", Color::RED, Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated invalid tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                sub_messages.push_back({"tool", loop_warning});
                continue;
            }

            if (!quiet) {
                printf("%s❌ [sub-agent felaktig JSON i verktygsanrop / Invalid JSON in tool call: %s]%s\n",
                       Color::RED, parse_error.c_str(), Color::RESET);
            }
            const std::string tool_message = "<tool_response>\nerror: " + parse_error + "\n</tool_response>";
            sub_messages.push_back({"tool", tool_message});
            continue;
        }

        final_answer = strip_think_tags(response);
        if (!quiet) {
            printf("%s🤖 [sub-agent completed task successfully]%s\n", Color::GREEN, Color::RESET);
        }
        return final_answer;
    }

    if (!quiet) {
        printf("%s🤖 [sub-agent iteration limit reached]%s\n", Color::YELLOW, Color::RESET);
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
            printf("%s\n", res.c_str());
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
                printf("%s[agent] Context usage high (%d / %d tokens, %.1f%%). Automatically compacting context...%s\n",
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
                    printf("%s[agent] Context usage high (%d / %d tokens, %.1f%%). Automatically compacting context...%s\n",
                           Color::YELLOW, used_ctx, n_ctx, pct, Color::RESET);
                }
                compact_context(backend, messages, quiet);
            }
        }

        ThinkingStreamFilter stream_filter([quiet](std::string_view piece, bool is_thinking) {
            if (!quiet) {
                if (is_thinking) {
                    printf("%s%.*s%s", Color::DIM, static_cast<int>(piece.size()), piece.data(), Color::RESET);
                } else {
                    printf("%s%.*s", Color::YELLOW, static_cast<int>(piece.size()), piece.data());
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
            printf("%s\n", Color::RESET);
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
                    printf("%s[agent loop detected: skipping duplicate tool call]%s\n", Color::RED, Color::RESET);
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
                            printf("%s[agent] Alltid godkänn aktiverat: efterföljande verktygsanrop tillåts automatiskt.%s\n",
                                   Color::GREEN, Color::RESET);
                        }
                    } else if (approval == ToolApproval::DENY) {
                        if (!quiet) {
                            printf("%s❌ [Verktygskörning nekades av användaren / Tool execution denied by user: %s]%s\n",
                                   Color::RED, tc.name.c_str(), Color::RESET);
                        }
                        std::string denial_msg = "<tool_response>\nerror: tool execution was denied by the user for tool '" + tc.name + "'.\n</tool_response>";
                        messages.push_back({"tool", denial_msg});
                        stop_execution = true;
                        break;
                    }
                }

                if (!quiet) {
                    if (tool_calls.size() > 1) {
                        printf("%s⚙️  [Agent Action (%zu/%zu): %s%s%s]%s\n",
                               Color::CYAN, idx + 1, tool_calls.size(), Color::BOLD, tc.name.c_str(), Color::CYAN, Color::RESET);
                    } else {
                        printf("%s⚙️  [Agent Action: %s%s%s]%s\n",
                               Color::CYAN, Color::BOLD, tc.name.c_str(), Color::CYAN, Color::RESET);
                    }
                    printf("%s   Args: %s%s\n", Color::GRAY, tc.arguments.dump(2).c_str(), Color::RESET);
                }

                const std::string tool_result = run_tool(tools, tc.name, tc.arguments);

                if (!quiet) {
                    std::string preview = tool_result;
                    if (preview.size() > 180) {
                        preview = preview.substr(0, 180) + "...";
                    }
                    std::replace(preview.begin(), preview.end(), '\n', ' ');
                    printf("%s📋 [Observation (%zu chars): %s]%s\n",
                   Color::MAGENTA, tool_result.size(), preview.c_str(), Color::RESET);
                }

                const std::string tool_message = "<tool_response>\n" + tool_result + "\n</tool_response>";
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
                    printf("%s[agent loop detected: skipping duplicate invalid tool call]%s\n", Color::RED, Color::RESET);
                }
                std::string loop_warning = "<tool_response>\nerror: repeated invalid tool call detected. Please provide your final answer or try a different approach.\n</tool_response>";
                messages.push_back({"tool", loop_warning});
                continue;
            }

            if (!quiet) {
                printf("%s❌ [Felaktig JSON i verktygsanrop / Invalid JSON in tool call: %s]%s\n",
                       Color::RED, parse_error.c_str(), Color::RESET);
            }
            const std::string tool_message = "<tool_response>\nerror: " + parse_error + "\n</tool_response>";
            messages.push_back({"tool", tool_message});
            continue;
        }

        // No tool call -> final answer reached
        break;
    }

    if (quiet) {
        std::string cleaned = strip_think_tags(final_turn_response);
        printf("%s\n", cleaned.c_str());
    } else {
        int used_ctx = backend.get_used_context();
        int n_ctx = backend.get_context_size();
        if (n_ctx > 0 && used_ctx > 0) {
            float pct = Context::get_usage_percentage(used_ctx, n_ctx);
            printf("%s📊 [Context: %d / %d tokens (%.1f%%)]%s\n", Color::DIM, used_ctx, n_ctx, pct, Color::RESET);
        }
        printf("\n");
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

    printf("%s=======================================================%s\n", Color::CYAN, Color::RESET);
    printf("%s        %.*s         %s\n", Color::BOLD, static_cast<int>(mode_title.size()), mode_title.data(), Color::RESET);
    if (!extra_info.empty()) {
        printf("  %.*s\n", static_cast<int>(extra_info.size()), extra_info.data());
    }
    printf("  Tools registered: %zu | Sub-agents: %s | Approval: %s | Context: %d | Temp: %.2f\n",
           tools.size(), config.enable_subagents ? "ENABLED" : "DISABLED",
           config.auto_approve ? "AUTO-APPROVE" : "REQUIRE APPROVAL",
           backend.get_context_size(), config.temperature);
    printf("  Type %s/help%s for commands, %s/tools%s to list available tools\n",
           Color::GREEN, Color::RESET, Color::GREEN, Color::RESET);
    printf("%s=======================================================%s\n\n", Color::CYAN, Color::RESET);

    while (true) {
        printf("%s%sagent> %s", Color::BOLD, Color::GREEN, Color::RESET);
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
            printf("%sExiting agent session.%s\n", Color::YELLOW, Color::RESET);
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
            printf("%s[agent] Sub-agents are now %s.%s\n\n",
                   Color::CYAN, config.enable_subagents ? "ENABLED" : "DISABLED", Color::RESET);
            continue;
        }

        if (user_input == "/approval" || user_input == "/approve" || user_input == "/confirm") {
            config.auto_approve = !config.auto_approve;
            printf("%s[agent] Tool approval mode: %s%s\n\n",
                   Color::CYAN, config.auto_approve ? "AUTO-APPROVE (always allow)" : "REQUIRE APPROVAL (prompt ja/nej/alltid ja)", Color::RESET);
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
            printf("\n%sRegistered Agent Tools:%s\n", Color::BOLD, Color::RESET);
            for (const auto & tool : tools) {
                printf("\n%s• %s%s\n", Color::CYAN, tool.name.c_str(), Color::RESET);
                printf("  %sDescription:%s %s\n", Color::DIM, Color::RESET, tool.description.c_str());
                printf("  %sSchema:%s\n    %s\n", Color::DIM, Color::RESET, tool.schema_doc.c_str());
            }
            printf("\n");
            continue;
        }

        if (user_input == "/system") {
            printf("\n%sCurrent System Prompt:%s\n%s%s%s\n\n", Color::BOLD, Color::RESET, Color::GRAY, system_prompt.c_str(), Color::RESET);
            continue;
        }

        if (user_input == "/clear" || user_input == "/reset") {
            messages.clear();
            messages.push_back({"system", system_prompt});
            backend.reset_context();
            printf("%s[agent] Conversation history and context memory reset.%s\n\n", Color::GREEN, Color::RESET);
            continue;
        }

        execute_agent_turn(backend, messages, tools, user_input,
                           config.temperature, config.max_iterations, config.auto_approve,
                           config.allowed_tools, nullptr, config.quiet);
    }

    return 0;
}
