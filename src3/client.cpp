#include "client/config.hpp"
#include "client/rest_client.hpp"
#include "common/defer.hpp"
#include "common/log.hpp"

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

/** Poll until session is not generating (job callback may lag stream EOF slightly). */
SessionResponse wait_session_ready(RestClient &client, const SessionID &session_id, int max_ms = 5000)
{
    const int step = 50;
    int waited = 0;
    SessionResponse last;
    while (waited <= max_ms)
    {
        last = client.get_session(session_id);
        if (last.state.is_sleeping() && !last.active_job_key.has_value())
        {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        waited += step;
    }
    return last;
}

std::string stream_job(RestClient &client, const SessionID &session_id, const JobKey job_key, bool quiet)
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

    bool thinking = false;
    printf("💭%s", Color::GRAY);
    std::string text = client.stream_tokens(session_id, job_key, [&](const string& piece) {
        //filter.process(piece);
            string_view str(piece);
            if (thinking)
            {
                const auto idx = str.find("</think>");
                if (idx != std::string_view::npos)
                {
                    thinking = false;
                    str = str.substr(0, idx);
                }
            }
            else
            {
                const auto idx = str.find("<think>");
                if (idx != std::string_view::npos)
                {
                    thinking = true;
                    str = str.substr(idx + 7);
                }
            }
        printf("%.*s", static_cast<int>(str.size()), str.data());
        fflush(stdout);
        return true;
    });
    //filter.flush();
    if (!quiet)
    {
        printf("%s\n", Color::RESET);
        fflush(stdout);
    }
    return text;
}

json run_pending_tools(const SessionResponse &session, span<const Tool> tools, CliConfig &cfg)
{
    json results = json::array();
    if (session.pending_tool_calls.empty())
    {
        return results;
    }

    for (const auto &tc : session.pending_tool_calls)
    {
        const string id = tc.id;
        const string name = tc.name;
        json args = tc.arguments;
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
                    printf("%s[client] auto-approve enabled for remaining tools%s\n", Color::GREEN, Color::RESET);
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
            printf("%s⚙️  [tool: %s%s%s]%s\n", Color::CYAN, Color::BOLD, name.c_str(), Color::CYAN, Color::RESET);
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
            printf("%s📋 [%zu chars] %s%s\n", Color::MAGENTA, out.size(), preview.c_str(), Color::RESET);
        }
        results.push_back(std::move(item));
    }
    return results;
}

std::optional<std::string> prompt_question_answer(const SessionResponse &session)
{
    if (!session.pending_question.has_value())
    {
        return std::nullopt;
    }

    const auto &q = session.pending_question.value();
    const std::string text = q.text;
    std::vector<std::string> answers = q.answers;

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
        fprintf(stderr, "%sempty answer%s\n", Color::RED, Color::RESET);
        return std::nullopt;
    }
    return line;
}

/**
 * Drive one user turn: post message (or continue from existing job), stream, resolve
 * tools/questions until idle or rounds exhausted.
 */
bool drive_session_turn(RestClient &client, const SessionID &session_id, std::span<const Tool> tools, CliConfig &cfg,
                        const JobKey initial_job_key)
{
    JobKey job_key = initial_job_key;
    stream_job(client, session_id, job_key, cfg.quiet);
    for (int round = 0; round < cfg.max_tool_rounds; ++round)
    {
        auto session = wait_session_ready(client, session_id);
        if (session.state.is_running())
        {
            // Stream job output (thinking)
            stream_job(client, session_id, job_key, cfg.quiet);

            // Wait for the session to be ready for more input - if
            // idle then no more session specific input is required.
            // TODO: Move the question and answering out from this method
            session = wait_session_ready(client, session_id);
            if (session.state.value == SessionState::Idle)
            {
                return true;
            }
        }

        if (session.state.value == SessionState::Idle)
        {
            return true;
        }

        if (session.state.value == SessionState::AwaitingTools)
        {
            auto results = run_pending_tools(session, tools, cfg);
            if (results.empty())
            {
                fprintf(stderr, "%s[client] awaiting_tools but no tool_calls%s\n", Color::RED, Color::RESET);
                return false;
            }
            auto resp = client.post_tool_results(session_id, results);
            job_key = resp.value("key", JobKey());
            if (job_key == 0)
            {
                throw BadRequest{"required property 'key' is missing"};
            }
            continue;
        }

        if (session.state.value == SessionState::AwaitingQuestion)
        {
            auto ans = prompt_question_answer(session);
            if (!ans)
            {
                return false;
            }
            const auto resp = client.post_message(session_id,
                                            SessionMessageRequest{.content = *ans, .role = ChatMessage::ROLE_USER});
            job_key = resp.key;
            continue;
        }

        return false;
    }
    fprintf(stderr, "%s[client] max tool/question rounds reached%s\n", Color::RED, Color::RESET);
    return false;
}

int run_oneshot(RestClient &client, CliConfig &cfg)
{
    auto created =
        client.create_session(CreateSessionRequest{.system = cfg.system_prompt, .messages = {}, .questions = false});
    const SessionID session_id = created.id;
    defer(client.delete_session(session_id));

    try
    {
        auto resp = client.post_message(
            session_id, SessionMessageRequest{.content = cfg.single_command, .role = ChatMessage::ROLE_USER});
        const auto text = stream_job(client, session_id, resp.key, cfg.quiet);
        if (cfg.quiet)
        {
            printf("%s\n", strip_think_tags(text).c_str());
        }
        return 0;
    }
    catch (const BadRequest &e)
    {
        log_error(e.what());
        return 1;
    }
    catch (...)
    {
        throw;
    }
}

int run_session_mode(RestClient &client, CliConfig &cfg, std::span<const Tool> tools)
{
    auto created = client.create_session(
        CreateSessionRequest{.system = cfg.system_prompt, .messages = {}, .questions = cfg.questions});
    const SessionID session_id = created.id;

    if (!cfg.quiet)
    {
        printf("%s[client] session %lud at %s (questions=%s)%s\n", Color::CYAN, session_id, client.base_url().c_str(),
               cfg.questions ? "true" : "false", Color::RESET);
    }

    defer(client.delete_session(session_id));

    auto handle_user_text = [&](const std::string &text) -> bool {
        if (text.empty())
        {
            return true;
        }
        try
        {
            const auto resp =
                client.post_message(session_id, SessionMessageRequest{.content = text, .role = ChatMessage::ROLE_USER});
            return drive_session_turn(client, session_id, tools, cfg, resp.key);
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

                for (auto it = s.messages.rbegin(); it != s.messages.rend(); ++it)
                {
                    if (it->role == "assistant")
                    {
                        printf("%s\n", strip_think_tags(it->content).c_str());
                        break;
                    }
                }
            }
            catch (...)
            {
            }
        }
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
                printf("%s\n", s.to_json().dump(2).c_str());
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
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    std::setlocale(LC_NUMERIC, "C");

    CliConfig cfg;
    try
    {
        cfg = CliConfig::from_args(argc, argv);
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
        fprintf(stderr, "%s[client] server not reachable at %s (/health)%s\n", Color::RED, client.base_url().c_str(),
                Color::RESET);
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
    catch (const RestClient::ClientError &e)
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
