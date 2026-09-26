#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
    if (arg.starts_with("--allow-tool=")) {
        constexpr std::string_view prefix = "--allow-tool=";
        config.allowed_tools.emplace_back(arg.substr(prefix.size()));
        return true;
    }
    if (arg.starts_with("--allow=")) {
        constexpr std::string_view prefix = "--allow=";
        config.allowed_tools.emplace_back(arg.substr(prefix.size()));
        return true;
    }
    if ((arg == "--allow-tools" || arg == "--allowed-tools") && i + 1 < argc) {
        config.allowed_tools.append_range(parse_allowed_tools(argv[++i]));
        return true;
    }
    if (arg.starts_with("--allow-tools=")) {
        constexpr std::string_view prefix = "--allow-tools=";
        config.allowed_tools.append_range(parse_allowed_tools(arg.substr(prefix.size())));
        return true;
    }
    if (arg.starts_with("--allowed-tools=")) {
        constexpr std::string_view prefix = "--allowed-tools=";
        config.allowed_tools.append_range(parse_allowed_tools(arg.substr(prefix.size())));
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
    if (arg == "--chat-template" && i + 1 < argc) {
        config.template_path = argv[++i];
        return true;
    }
    if (arg == "--reasoning") {
        config.reasoning = true;
        return true;
    }
    if (arg == "--no-reasoning") {
        config.reasoning = false;
        return true;
    }
    if (arg == "--threads" && i + 1 < argc) {
        config.n_threads = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "--threads-batch" && i + 1 < argc) {
        config.n_threads_batch = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "--flash-attn" && i + 1 < argc) {
        config.flash_attn = argv[++i];
        return true;
    }
    if (arg == "--cache-type-k" && i + 1 < argc) {
        config.cache_type_k = argv[++i];
        return true;
    }
    if (arg == "--cache-type-v" && i + 1 < argc) {
        config.cache_type_v = argv[++i];
        return true;
    }
    if (arg == "--seed" && i + 1 < argc) {
        config.seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        return true;
    }
    if (arg == "--penalty-last-n" && i + 1 < argc) {
        config.penalty_last_n = std::stoi(argv[++i]);
        return true;
    }
    if (arg == "--frequency-penalty" && i + 1 < argc) {
        config.frequency_penalty = std::stof(argv[++i]);
        return true;
    }
    if (arg == "--max-tokens" && i + 1 < argc) {
        config.max_tokens = std::stoi(argv[++i]);
        return true;
    }
    return false;
}
