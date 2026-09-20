#include "client/rest_client.hpp"
#include "common/color.hpp"
#include "common/tools.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <curl/curl.h>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct CliConfig
{
    std::string host = "127.0.0.1";
    int port = 8080;
    bool questions = true;
    bool auto_approve = false;
    bool quiet = false;
    bool no_session = false; // one-shot /v1/messages
    std::string system_prompt;
    std::string single_command;
    std::vector<std::string> allowed_tools;
    int max_tool_rounds = 40;
};

void print_usage(const char *argv0)
{
    printf("\n%sCallisto REST Client%s\n", Color::BOLD, Color::RESET);
    printf("Talks to callisto_server over HTTP (/v1/sessions, /v1/messages).\n\n");
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
    printf("    --oneshot             Use POST /v1/messages only (no session/tools loop)\n");
    printf("    -it <n>               Max tool/question rounds per user turn (default: 40)\n");
    printf("    -h, --help            Show help\n\n");
    printf("Interactive commands (session mode):\n");
    printf("    /exit, /quit          End session\n");
    printf("    /state                Print session JSON state\n");
    printf("    /tools                List local tools\n");
    printf("    /approval             Toggle auto-approve\n");
    printf("    /help                 This help\n\n");
}

bool parse_args(int argc, char **argv, CliConfig &cfg)
{
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
        else if (arg == "-c" || arg == "-e" || arg == "--command" || arg == "--exec" ||
                 arg == "--prompt")
        {
            cfg.single_command = need(arg.c_str());
        }
        else if (arg == "-s")
        {
            cfg.system_prompt = need("-s");
        }
        else if (arg == "-q" || arg == "--quiet" || arg == "--silent")
        {
            cfg.quiet = true;
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
        else if (arg == "--oneshot" || arg == "--no-session")
        {
            cfg.no_session = true;
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
        else if (!arg.empty() && arg[0] != '-')
        {
            if (!cfg.single_command.empty())
            {
                cfg.single_command += " ";
            }
            cfg.single_command += arg;
        }
        else
        {
            fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

/** Poll until session is not generating (job callback may lag stream EOF slightly). */
nlohmann::json wait_session_ready(RestClient &client, const std::string &session_id, int max_ms = 5000)
{
    const int step = 50;
    int waited = 0;
    nlohmann::json last;
    while (waited <= max_ms)
    {
        last = client.get_session(session_id);
        const std::string state = last.value("state", "");
        if (state != "generating" && !last.contains("active_job_key"))
        {
            return last;
        }
        if (state != "generating" && last.value("active_job_key", "").empty())
        {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    return last;
}

std::string stream_job(RestClient &client, const std::string &job_key, bool quiet)
{
    ThinkingStreamFilter filter([quiet](std::string_view piece, bool is_thinking) {
        if (quiet)
        {
            return;
        }
        if (is_thinking)
        {
            printf("%s%.*s%s", Color::DIM, static_cast<int>(piece.size()), piece.data(), Color::RESET);
        }
        else
        {
            printf("%s%.*s", Color::RESET, static_cast<int>(piece.size()), piece.data());
        }
        fflush(stdout);
    });

    std::string text = client.stream_tokens(job_key, [&](std::string_view piece) {
        filter.process(piece);
        return true;
    });
    filter.flush();
    if (!quiet)
    {
        printf("%s\n", Color::RESET);
        fflush(stdout);
    }
    return text;
}

json run_pending_tools(const json &session, span<const Tool> tools, CliConfig &cfg)
{
    json results = json::array();
    if (!session.contains("tool_calls") || !session["tool_calls"].is_array())
    {
        return results;
    }

    for (const auto &tc : session["tool_calls"])
    {
        const string id = tc.value("id", "");
        const string name = tc.value("name", "");
        json args = tc.value("arguments", json::object());
        if (!args.is_object())
        {
            args = json::object();
        }

        json item{{"id", id}, {"name", name}, {"denied", false}, {"content", ""}};

        if (!is_tool_allowed(name, cfg.auto_approve, cfg.allowed_tools))
        {
            ToolApproval approval = prompt_tool_approval(name, args);
            if (approval == ToolApproval::ALWAYS)
            {
                cfg.auto_approve = true;
                if (!cfg.quiet)
                {
                    printf("%s[client] auto-approve enabled for remaining tools%s\n", Color::GREEN,
                           Color::RESET);
                }
            }
            else if (approval == ToolApproval::DENY)
            {
                item["denied"] = true;
                item["content"] = "denied by user";
                if (!cfg.quiet)
                {
                    printf("%s❌ tool denied: %s%s\n", Color::RED, name.c_str(), Color::RESET);
                }
                results.push_back(std::move(item));
                continue;
            }
        }

        if (!cfg.quiet)
        {
            printf("%s⚙️  [tool: %s%s%s]%s\n", Color::CYAN, Color::BOLD, name.c_str(), Color::CYAN,
                   Color::RESET);
            printf("%s   args: %s%s\n", Color::GRAY, args.dump().c_str(), Color::RESET);
        }

        const std::string out = run_tool(tools, name, args);
        item["content"] = out;

        if (!cfg.quiet)
        {
            std::string preview = out;
            if (preview.size() > 200)
            {
                preview = preview.substr(0, 200) + "...";
            }
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            printf("%s📋 [%zu chars] %s%s\n", Color::MAGENTA, out.size(), preview.c_str(),
                   Color::RESET);
        }
        results.push_back(std::move(item));
    }
    return results;
}

std::optional<std::string> prompt_question_answer(const nlohmann::json &session, bool quiet)
{
    if (!session.contains("question") || !session["question"].is_object())
    {
        return std::nullopt;
    }
    const auto &q = session["question"];
    const std::string text = q.value("text", "");
    std::vector<std::string> answers;
    if (q.contains("answers") && q["answers"].is_array())
    {
        for (const auto &a : q["answers"])
        {
            if (a.is_string())
            {
                answers.push_back(a.get<std::string>());
            }
        }
    }

    if (!quiet)
    {
        printf("\n%s❓ %s%s\n", Color::YELLOW, text.c_str(), Color::RESET);
        if (!answers.empty())
        {
            for (size_t i = 0; i < answers.size(); ++i)
            {
                printf("  %s[%zu]%s %s\n", Color::CYAN, i, Color::RESET, answers[i].c_str());
            }
            printf("%sChoose index, or type a free-form answer:%s ", Color::BOLD, Color::RESET);
        }
        else
        {
            printf("%sYour answer:%s ", Color::BOLD, Color::RESET);
        }
        fflush(stdout);
    }

    std::string line;
    if (!std::getline(std::cin, line))
    {
        return std::nullopt;
    }
    // trim
    auto first = line.find_first_not_of(" \t\r\n");
    auto last = line.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        line.clear();
    }
    else
    {
        line = line.substr(first, last - first + 1);
    }

    if (!answers.empty())
    {
        // pure integer index?
        bool all_digit = !line.empty() && std::all_of(line.begin(), line.end(), ::isdigit);
        if (all_digit)
        {
            int idx = std::stoi(line);
            if (idx >= 0 && static_cast<size_t>(idx) < answers.size())
            {
                return answers[static_cast<size_t>(idx)];
            }
        }
    }
    if (line.empty())
    {
        if (!quiet)
        {
            fprintf(stderr, "%sempty answer%s\n", Color::RED, Color::RESET);
        }
        return std::nullopt;
    }
    return line;
}

/**
 * Drive one user turn: post message (or continue from existing job), stream, resolve
 * tools/questions until idle or rounds exhausted.
 */
bool drive_session_turn(RestClient &client, const std::string &session_id, std::span<const Tool> tools,
                        CliConfig &cfg, const std::string *initial_job_key = nullptr)
{
    std::string job_key = initial_job_key ? *initial_job_key : "";
    for (int round = 0; round < cfg.max_tool_rounds; ++round)
    {
        if (job_key.empty())
        {
            auto session = wait_session_ready(client, session_id);
            const std::string state = session.value("state", "idle");
            if (state == "idle")
            {
                return true;
            }
            if (state == "awaiting_tools")
            {
                auto results = run_pending_tools(session, tools, cfg);
                if (results.empty())
                {
                    fprintf(stderr, "%s[client] awaiting_tools but no tool_calls%s\n", Color::RED,
                            Color::RESET);
                    return false;
                }
                auto resp = client.post_tool_results(session_id, results);
                job_key = resp.value("key", "");
                continue;
            }
            if (state == "awaiting_question")
            {
                auto ans = prompt_question_answer(session, cfg.quiet);
                if (!ans)
                {
                    return false;
                }
                auto resp = client.post_message(session_id, *ans);
                job_key = resp.value("key", "");
                continue;
            }
            fprintf(stderr, "%s[client] unexpected session state: %s%s\n", Color::RED, state.c_str(),
                    Color::RESET);
            return false;
        }

        stream_job(client, job_key, cfg.quiet);
        job_key.clear();

        // Optional: surface job error
        try
        {
            // job may still exist briefly
        }
        catch (...)
        {
        }

        auto session = wait_session_ready(client, session_id);
        const std::string state = session.value("state", "idle");
        if (state == "idle")
        {
            return true;
        }
        // loop continues to handle awaiting_* without a job key
    }
    fprintf(stderr, "%s[client] max tool/question rounds reached%s\n", Color::RED, Color::RESET);
    return false;
}

int run_oneshot(RestClient &client, CliConfig &cfg)
{
    nlohmann::json body;
    body["system"] = cfg.system_prompt;
    body["messages"] = nlohmann::json::array({nlohmann::json{{"role", "user"}, {"content", cfg.single_command}}});

    auto resp = client.post_messages(body);
    const std::string key = resp.value("key", "");
    if (key.empty())
    {
        fprintf(stderr, "no job key returned\n");
        return 1;
    }
    std::string text = stream_job(client, key, cfg.quiet);
    if (cfg.quiet)
    {
        // still print final (without think tags)
        printf("%s\n", strip_think_tags(text).c_str());
    }
    return 0;
}

int run_session_mode(RestClient &client, CliConfig &cfg, std::span<const Tool> tools)
{
    json create_body{
        {"questions", cfg.questions},
        {"messages", json::array()},
    };
    if (!cfg.system_prompt.empty())
    {
        create_body["system"] = cfg.system_prompt;
    }

    auto created = client.create_session(create_body);
    const std::string session_id = created.value("id", "");
    if (session_id.empty())
    {
        fprintf(stderr, "create session: missing id\n");
        return 1;
    }

    if (!cfg.quiet)
    {
        printf("%s[client] session %s at %s (questions=%s)%s\n", Color::CYAN,
               session_id.c_str(), client.base_url().c_str(),
               cfg.questions ? "true" : "false", Color::RESET);
    }

    auto cleanup = [&]() {
        try
        {
            client.delete_session(session_id);
        }
        catch (...)
        {
        }
    };

    auto handle_user_text = [&](const std::string &text) -> bool {
        if (text.empty())
        {
            return true;
        }
        try
        {
            auto resp = client.post_message(session_id, text);
            std::string key = resp.value("key", "");
            return drive_session_turn(client, session_id, tools, cfg, &key);
        }
        catch (const std::exception &e)
        {
            fprintf(stderr, "%s[client] %s%s\n", Color::RED, e.what(), Color::RESET);
            return false;
        }
    };

    int rc = 0;
    if (!cfg.single_command.empty())
    {
        if (!handle_user_text(cfg.single_command))
        {
            rc = 1;
        }
        else if (cfg.quiet)
        {
            // Print last assistant message from session
            try
            {
                auto s = client.get_session(session_id);
                if (s.contains("messages") && s["messages"].is_array())
                {
                    for (auto it = s["messages"].rbegin(); it != s["messages"].rend(); ++it)
                    {
                        if (it->value("role", "") == "assistant")
                        {
                            printf("%s\n", strip_think_tags(it->value("content", "")).c_str());
                            break;
                        }
                    }
                }
            }
            catch (...)
            {
            }
        }
        cleanup();
        return rc;
    }

    // Interactive REPL
    if (!cfg.quiet)
    {
        printf("%sType a message, or /help. Ctrl-D or /exit to quit.%s\n\n", Color::DIM, Color::RESET);
    }

    std::string line;
    while (true)
    {
        if (!cfg.quiet)
        {
            printf("%syou>%s ", Color::BOLD, Color::RESET);
            fflush(stdout);
        }
        if (!std::getline(std::cin, line))
        {
            printf("\n");
            break;
        }
        auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            continue;
        }
        line = line.substr(first);
        auto last = line.find_last_not_of(" \t\r\n");
        if (last != std::string::npos)
        {
            line = line.substr(0, last + 1);
        }

        if (line == "/exit" || line == "/quit")
        {
            break;
        }
        if (line == "/help")
        {
            print_usage("callisto_client");
            continue;
        }
        if (line == "/state")
        {
            try
            {
                auto s = client.get_session(session_id);
                printf("%s\n", s.dump(2).c_str());
            }
            catch (const std::exception &e)
            {
                fprintf(stderr, "%s\n", e.what());
            }
            continue;
        }
        if (line == "/tools")
        {
            for (const auto &t : tools)
            {
                printf("  - %s\n", t.name.c_str());
            }
            continue;
        }
        if (line == "/approval")
        {
            cfg.auto_approve = !cfg.auto_approve;
            printf("auto-approve: %s\n", cfg.auto_approve ? "on" : "off");
            continue;
        }

        handle_user_text(line);
        if (!cfg.quiet)
        {
            printf("\n");
        }
    }

    cleanup();
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::setlocale(LC_NUMERIC, "C");

    CliConfig cfg;
    try
    {
        if (!parse_args(argc, argv, cfg))
        {
            print_usage(argv[0]);
            curl_global_cleanup();
            return 1;
        }
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "cli error: %s\n", e.what());
        print_usage(argv[0]);
        curl_global_cleanup();
        return 1;
    }

    if (cfg.port <= 0 || cfg.port > 65535)
    {
        fprintf(stderr, "invalid port\n");
        curl_global_cleanup();
        return 1;
    }

    RestClient client(cfg.host, cfg.port);

    if (!cfg.quiet)
    {
        printf("%s[client] checking %s ...%s\n", Color::CYAN, client.base_url().c_str(), Color::RESET);
    }
    if (!client.health())
    {
        fprintf(stderr, "%s[client] server not reachable at %s (/health)%s\n", Color::RED,
                client.base_url().c_str(), Color::RESET);
        curl_global_cleanup();
        return 1;
    }

    // Local tools only (server does not execute them). No nested sub-agent backend for now.
    auto tools = get_registered_tools(false, nullptr);

    int rc = 0;
    try
    {
        if (cfg.no_session)
        {
            if (cfg.single_command.empty())
            {
                fprintf(stderr, "--oneshot requires a prompt (-c or trailing args)\n");
                rc = 1;
            }
            else
            {
                rc = run_oneshot(client, cfg);
            }
        }
        else
        {
            rc = run_session_mode(client, cfg, tools);
        }
    }
    catch (const RestClient::Error &e)
    {
        fprintf(stderr, "%s[http %d] %s%s\n", Color::RED, e.status, e.what(), Color::RESET);
        rc = 1;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "%s[client] %s%s\n", Color::RED, e.what(), Color::RESET);
        rc = 1;
    }

    return rc;
}
