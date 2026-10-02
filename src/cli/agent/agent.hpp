#pragma once

#include "api/sessions.hpp"
#include "cli/servers.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum class ApprovalMode
{
    ReadOnly,
    Auto,
    Full
};

struct AgentConfig
{
    std::string host = "127.0.0.1";
    int port = 8080;
    ApprovalMode approval = ApprovalMode::ReadOnly;
    bool show_think = true;
    bool debug = false;
    bool questions = true;
    // New sessions shorten finished tool results. A resumed session keeps its own setting.
    bool compress_tools = true;
    bool exec = false;
    bool resume = false;
    SessionID session = 0;
    std::string prompt;
    // Empty loads $XDG_CONFIG_HOME/callisto/theme.json, or the built-in default theme.
    std::string theme;
    // Empty means the single --host and --port server.
    std::vector<ServerTarget> servers;
};

// Interactive agent, or one non-interactive task when exec is set.
// Returns 0 on success, 2 when the context was full, 1 on any other failure.
int run_agent(const AgentConfig &config);
