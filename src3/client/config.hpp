#pragma once

#include "../common/std.hpp"
#include "common/tools.hpp"
#include "common/color.hpp"

#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

inline void print_usage(const char *argv0)
{
    std::print("\n{}Callisto REST Client{}\n"
               "Talks to callisto_server over HTTP (/v1/sessions).\n\n"
               "Usage:\n"
               "    {} [options] [prompt...]\n\n"
               "Options:\n"
               "    --host <host>         Server host (default: 127.0.0.1)\n"
               "    -p, --port <int>      Server port (default: 8080)\n"
               "    -c, -e, --command <s> Single prompt, print result, exit\n"
               "    -s <prompt>           Extra system text (sent on session create)\n"
               "    -q, --quiet           Only print assistant output / final result\n"
               "    -y, --yes             Auto-approve all tool calls\n"
               "    --allow-tool <name>   Auto-approve one tool (repeatable)\n"
               "    --allow-tools <list>  Comma-separated auto-approve list\n"
               "    --no-questions        Create session with questions:false\n"
               "    --questions           Create session with questions:true (default)\n"
               "    --show-think          Print model <think> text (hidden by default)\n"
               "    --oneshot             Temp session: one message, no tools loop, then delete\n"
               "    --sub-agents          Allow the sub_agent tool (default)\n"
               "    --no-sub-agents       Do not offer sub_agent\n"
               "    -it <n>               Max tool/question rounds per user turn (default: 40)\n"
               "    -h, --help            Show help\n\n"
               "Interactive commands (session mode):\n"
               "    /exit, /quit          End session\n"
               "    /state                Print session JSON state\n"
               "    /tools                List local tools\n"
               "    /approval             Toggle auto-approve\n"
               "    /think                Toggle printing <think> text\n"
               "    /help                 This help\n\n",
               Color::BOLD, Color::RESET, argv0);
}

struct CliConfig
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool questions = true;
    bool auto_approve = false;
    std::string system_prompt;
    std::vector<std::string> allowed_tools;
    int max_tool_rounds = 40;
    bool show_think = false;
    bool subagents = true;

    static CliConfig from_args(int argc, char **argv)
    {
        CliConfig cfg;
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            auto need = [&](const char *name) -> const char * {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error(std::format("missing value for {}", name));
                }
                return argv[++i];
            };

            if (arg == "--host")
            {
                cfg.host = need("--host");
            }
            else if (arg == "-p" || arg == "--port")
            {
                cfg.port = std::stoi(need(arg.c_str()));
            }
            else if (arg == "-s")
            {
                cfg.system_prompt = need("-s");
            }
            else if (arg == "-y" || arg == "--yes" || arg == "--auto-approve")
            {
                cfg.auto_approve = true;
            }
            else if (arg == "--allow-tool" || arg == "--allow")
            {
                cfg.allowed_tools.push_back(need(arg.c_str()));
            }
            else if (arg.starts_with("--allow-tool="))
            {
                constexpr std::string_view prefix = "--allow-tool=";
                cfg.allowed_tools.emplace_back(arg.substr(prefix.size()));
            }
            else if (arg == "--allow-tools" || arg == "--allowed-tools")
            {
                cfg.allowed_tools.append_range(parse_allowed_tools(need(arg.c_str())));
            }
            else if (arg == "--no-questions")
            {
                cfg.questions = false;
            }
            else if (arg == "--questions")
            {
                cfg.questions = true;
            }
            else if (arg == "--show-think")
            {
                cfg.show_think = true;
            }
            else if (arg == "--sub-agents" || arg == "--subagents" || arg == "-sa")
            {
                cfg.subagents = true;
            }
            else if (arg == "--no-sub-agents" || arg == "--no-subagents")
            {
                cfg.subagents = false;
            }
            else if (arg == "-it" && i + 1 < argc)
            {
                cfg.max_tool_rounds = std::stoi(need("-it"));
            }
            else if (arg == "-h" || arg == "--help")
            {
                print_usage(argv[0]);
                std::exit(0);
            }
            else
            {
                throw std::runtime_error{std::format("unknown argument: {}", arg)};
            }
        }
        return cfg;
    }
};
