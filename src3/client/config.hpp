#pragma once

#include "../common/std.hpp"
#include "common/tools.hpp"
#include "common/color.hpp"

void print_usage(const char *argv0)
{
    printf("\n%sCallisto REST Client%s\n", Color::BOLD, Color::RESET);
    printf("Talks to callisto_server over HTTP (/v1/sessions).\n\n");
    printf("Usage:\n");
    printf("    %s [options] [prompt...]\n\n", argv0);
    printf("Options:\n");
    printf("    --host <host>         Server host (default: 127.0.0.1)\n");
    printf("    -p, --port <int>      Server port (default: 8080)\n");
    printf("    -c, -e, --command <s> Single prompt, print result, exit\n");
    printf("    -s <prompt>           Extra system text (sent on session create)\n");
    printf("    -q, --quiet           Only print assistant output / final result\n");
    printf("    -y, --yes             Auto-approve all tool calls\n");
    printf("    --allow-tool <name>   Auto-approve one tool (repeatable)\n");
    printf("    --allow-tools <list>  Comma-separated auto-approve list\n");
    printf("    --no-questions        Create session with questions:false\n");
    printf("    --questions           Create session with questions:true (default)\n");
    printf("    --show-think          Print model <think> text (hidden by default)\n");
    printf("    --oneshot             Temp session: one message, no tools loop, then delete\n");
    printf("    -it <n>               Max tool/question rounds per user turn (default: 40)\n");
    printf("    -h, --help            Show help\n\n");
    printf("Interactive commands (session mode):\n");
    printf("    /exit, /quit          End session\n");
    printf("    /state                Print session JSON state\n");
    printf("    /tools                List local tools\n");
    printf("    /approval             Toggle auto-approve\n");
    printf("    /think                Toggle printing <think> text\n");
    printf("    /help                 This help\n\n");
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

    static CliConfig from_args(int argc, char **argv)
    {
        CliConfig cfg;
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            auto need = [&](const char *name) -> const char * {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error(std::string("missing value for ") + name);
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
            else if (arg.rfind("--allow-tool=", 0) == 0)
            {
                cfg.allowed_tools.push_back(arg.substr(13));
            }
            else if (arg == "--allow-tools" || arg == "--allowed-tools")
            {
                auto parsed = parse_allowed_tools(need(arg.c_str()));
                cfg.allowed_tools.insert(cfg.allowed_tools.end(), parsed.begin(), parsed.end());
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
                throw std::runtime_error{"unknown argument: " + arg};
            }
        }
        return cfg;
    }
};
