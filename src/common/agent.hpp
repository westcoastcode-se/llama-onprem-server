#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/agent_backend.hpp"
#include "common/protocol.hpp"
#include "common/tools.hpp"

inline constexpr int DEFAULT_MAX_AGENT_ITERATIONS = 25;

struct AgentSessionConfig {
    float temperature = 0.7f;
    int max_iterations = DEFAULT_MAX_AGENT_ITERATIONS;
    bool enable_subagents = true;
    bool auto_approve = false;
    bool quiet = false;
    std::vector<std::string> allowed_tools;
    std::string custom_system_prompt;
    std::string single_command;
};

// Print list of available interactive slash commands
void print_slash_commands_help();

// Compact context history using the backend's generation capability
bool compact_context(IAgentBackend & backend,
                    std::vector<Protocol::ChatMessage> & messages,
                    bool quiet = false);

// Run a subagent for a given delegated task
std::string run_subagent(IAgentBackend & backend,
                         std::string_view task,
                         std::span<const Tool> base_tools,
                         std::string_view custom_system_prompt,
                         float temperature,
                         int max_iterations,
                         bool & auto_approve,
                         std::span<const std::string> allowed_tools = {},
                         bool quiet = false);

// Execute a single turn of the agent (including ReAct loop)
bool execute_agent_turn(IAgentBackend & backend,
                        std::vector<Protocol::ChatMessage> & messages,
                        std::span<const Tool> tools,
                        std::string_view user_input,
                        float temperature,
                        int max_iterations,
                        bool & auto_approve,
                        std::span<const std::string> allowed_tools = {},
                        std::string * out_response = nullptr,
                        bool quiet = false);

// Run the full agent session (single command or interactive REPL)
int run_agent_session(IAgentBackend & backend,
                      AgentSessionConfig & config,
                      std::string_view mode_title,
                      std::string_view extra_info = "");
