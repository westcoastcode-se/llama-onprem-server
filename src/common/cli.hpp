#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include "common/agent.hpp"
#include "common/tools.hpp"

inline bool parse_network_cli_arg(int & i, int argc, char ** argv, std::string & host, int & port) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
        host = argv[++i];
        return true;
    }
    if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
        port = std::stoi(argv[++i]);
        return true;
    }
    return false;
}

inline bool parse_agent_cli_arg(int & i, int argc, char ** argv, AgentSessionConfig & config, bool allow_dash_c_for_command = false) {
    std::string arg = argv[i];
    if (arg == "-t" && i + 1 < argc) {
        config.temperature = std::stof(argv[++i]);
        return true;
    }
    if (arg == "-it" && i + 1 < argc) {
        config.max_iterations = std::stoi(argv[++i]);
        return true;
    }
    if ((arg == "--command" || arg == "-e" || arg == "--exec" || arg == "--prompt" || (allow_dash_c_for_command && arg == "-c")) && i + 1 < argc) {
        config.single_command = argv[++i];
        return true;
    }
    if (arg == "-q" || arg == "--quiet" || arg == "--silent") {
        config.quiet = true;
        return true;
    }
    if ((arg == "--allow-tool" || arg == "--allow") && i + 1 < argc) {
        config.allowed_tools.push_back(argv[++i]);
        return true;
    }
    if (arg.rfind("--allow-tool=", 0) == 0) {
        config.allowed_tools.push_back(arg.substr(13));
        return true;
    }
    if (arg.rfind("--allow=", 0) == 0) {
        config.allowed_tools.push_back(arg.substr(8));
        return true;
    }
    if ((arg == "--allow-tools" || arg == "--allowed-tools") && i + 1 < argc) {
        auto parsed = parse_allowed_tools(argv[++i]);
        config.allowed_tools.insert(config.allowed_tools.end(), parsed.begin(), parsed.end());
        return true;
    }
    if (arg.rfind("--allow-tools=", 0) == 0) {
        auto parsed = parse_allowed_tools(arg.substr(14));
        config.allowed_tools.insert(config.allowed_tools.end(), parsed.begin(), parsed.end());
        return true;
    }
    if (arg.rfind("--allowed-tools=", 0) == 0) {
        auto parsed = parse_allowed_tools(arg.substr(16));
        config.allowed_tools.insert(config.allowed_tools.end(), parsed.begin(), parsed.end());
        return true;
    }
    if (arg == "--sub-agents" || arg == "--subagents" || arg == "--subagent" || arg == "-sa") {
        config.enable_subagents = true;
        return true;
    }
    if (arg == "--no-sub-agents" || arg == "--no-subagents") {
        config.enable_subagents = false;
        return true;
    }
    if (arg == "-y" || arg == "--yes" || arg == "--auto-approve" || arg == "--always-allow-tools") {
        config.auto_approve = true;
        return true;
    }
    if (arg == "--require-approval" || arg == "--confirm-tools") {
        config.auto_approve = false;
        return true;
    }
    if (arg == "-s" && i + 1 < argc) {
        config.custom_system_prompt = argv[++i];
        return true;
    }
    if (!arg.empty() && arg[0] != '-') {
        if (!config.single_command.empty()) config.single_command += " ";
        config.single_command += arg;
        return true;
    }
    return false;
}

template <typename TConfig>
inline bool parse_llama_cli_arg(int & i, int argc, char ** argv, TConfig & config) {
    std::string arg = argv[i];
    if (arg == "-m" && i + 1 < argc) {
        config.model_path = argv[++i];
        return true;
    }
    if (arg == "-c" && i + 1 < argc) {
        config.n_ctx = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "-b" && i + 1 < argc) {
        config.n_batch = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "-ngl" && i + 1 < argc) {
        config.n_gpu_layers = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "-t" && i + 1 < argc) {
        config.temperature = std::stof(argv[++i]);
        return true;
    }
    return false;
}
