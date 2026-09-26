#include "client/config.hpp"
#include "client/rest_client.hpp"
#include "common/defer.hpp"
#include "common/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <curl/curl.h>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::atomic<bool> g_interrupt{false};

void on_sigint(int)
{
    g_interrupt.store(true, std::memory_order_relaxed);
}

void install_sigint()
{
    struct sigaction sa {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: getline returns on Ctrl-C
    sigaction(SIGINT, &sa, nullptr);
}

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

struct StreamResult
{
    SessionResponse session;
    bool cancelled = false;
};

StreamResult stream_job(RestClient &client, const SessionID &session_id, const JobKey job_key, bool show_think)
{
    std::string pending;
    bool thinking = false;
    bool think_started = false;
    bool think_closed = false;
    bool answer_started = false;
    bool status_line = false;
    int spin = 0;
    const auto t0 = std::chrono::steady_clock::now();

    auto elapsed_s = [&]() {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    };

    auto emit = [](std::string_view s) {
        if (!s.empty())
        {
            printf("%.*s", static_cast<int>(s.size()), s.data());
        }
    };

    auto clear_status = [&]() {
        if (status_line)
        {
            printf("\r\033[K");
            status_line = false;
        }
    };

    auto draw_thinking = [&]() {
        static constexpr const char *kFrames = "|/-\\";
        printf("\r%s%c Thinking… %.1fs%s\033[K", Color::DIM, kFrames[spin & 3], elapsed_s(), Color::RESET);
        status_line = true;
        ++spin;
        fflush(stdout);
    };

    auto emit_answer = [&](std::string_view s) {
        if (s.empty())
        {
            return;
        }
        if (!answer_started)
        {
            clear_status();
            if (think_started && !think_closed && !show_think)
            {
                printf("%s✓ Thinking  %.1fs%s\n", Color::DIM, elapsed_s(), Color::RESET);
                think_closed = true;
            }
            answer_started = true;
        }
        emit(s);
    };

    draw_thinking();

    g_interrupt.store(false, std::memory_order_relaxed);
    std::atomic<bool> stream_over{false};
    std::thread watcher([&]() {
        while (!stream_over.load(std::memory_order_relaxed))
        {
            if (g_interrupt.load(std::memory_order_relaxed))
            {
                client.stop();
                try
                {
                    RestClient killer(client.host(), client.port());
                    killer.cancel_job(session_id, job_key);
                }
                catch (...)
                {
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    bool cancelled = false;
    try
    {
        client.stream_tokens(session_id, job_key, [&](const string &piece) {
            if (g_interrupt.load(std::memory_order_relaxed))
            {
                return false;
            }
            pending += piece;
            constexpr std::string_view kOpen = "<think>";
            constexpr std::string_view kClose = "</think>";

            while (!pending.empty())
            {
                if (thinking)
                {
                    const auto idx = pending.find(kClose);
                    if (idx == std::string::npos)
                    {
                        const size_t keep = std::min(pending.size(), kClose.size() - 1);
                        const auto vis = std::string_view(pending).substr(0, pending.size() - keep);
                        if (show_think)
                        {
                            emit_answer(vis);
                        }
                        else
                        {
                            think_started = true;
                            draw_thinking();
                        }
                        pending.erase(0, pending.size() - keep);
                        break;
                    }
                    if (show_think)
                    {
                        emit_answer(std::string_view(pending).substr(0, idx));
                    }
                    pending.erase(0, idx + kClose.size());
                    thinking = false;
                    if (!show_think)
                    {
                        clear_status();
                        printf("%s✓ Thinking  %.1fs%s\n", Color::DIM, elapsed_s(), Color::RESET);
                        think_closed = true;
                    }
                    else
                    {
                        printf("%s", Color::RESET);
                    }
                }
                else
                {
                    const auto idx = pending.find(kOpen);
                    if (idx == std::string::npos)
                    {
                        const size_t keep = std::min(pending.size(), kOpen.size() - 1);
                        emit_answer(std::string_view(pending).substr(0, pending.size() - keep));
                        pending.erase(0, pending.size() - keep);
                        break;
                    }
                    emit_answer(std::string_view(pending).substr(0, idx));
                    pending.erase(0, idx + kOpen.size());
                    thinking = true;
                    think_started = true;
                    if (show_think)
                    {
                        if (!answer_started)
                        {
                            clear_status();
                            answer_started = true;
                        }
                        printf("%s", Color::GRAY);
                    }
                    else
                    {
                        draw_thinking();
                    }
                }
            }
            fflush(stdout);
            return !g_interrupt.load(std::memory_order_relaxed);
        });
        if (!pending.empty() && !thinking)
        {
            emit_answer(pending);
        }
        else if (!pending.empty() && show_think)
        {
            emit_answer(pending);
        }
    }
    catch (const RestClient::ClientError &)
    {
        cancelled = g_interrupt.load(std::memory_order_relaxed);
        if (!cancelled)
        {
            stream_over.store(true, std::memory_order_relaxed);
            watcher.join();
            throw;
        }
    }

    stream_over.store(true, std::memory_order_relaxed);
    watcher.join();

    if (g_interrupt.load(std::memory_order_relaxed))
    {
        cancelled = true;
    }

    if (cancelled)
    {
        clear_status();
        printf("%s⚠ cancelled%s\n", Color::YELLOW, Color::RESET);
    }
    else if (!answer_started && think_started && !think_closed && !show_think)
    {
        clear_status();
        printf("%s✓ Thinking  %.1fs%s\n", Color::DIM, elapsed_s(), Color::RESET);
    }
    else
    {
        clear_status();
        printf("%s\n", Color::RESET);
    }
    fflush(stdout);

    return StreamResult{wait_session_ready(client, session_id), cancelled};
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
                printf("%s[client] auto-approve enabled for remaining tools%s\n", Color::GREEN, Color::RESET);
            }
            else if (approval == ToolApproval::DENY)
            {
                item["denied"] = true;
                item["content"] = "denied by user";
                printf("%s❌ tool denied: %s%s\n", Color::RED, name.c_str(), Color::RESET);
                results.push_back(std::move(item));
                continue;
            }
        }

        printf("%s⚙️  [tool: %s%s%s]%s\n", Color::CYAN, Color::BOLD, name.c_str(), Color::CYAN, Color::RESET);
        printf("%s   args: %s%s\n", Color::GRAY, args.dump().c_str(), Color::RESET);

        const std::string out = run_tool(tools, name, args);
        item["content"] = out;

        std::string preview = out;
        if (preview.size() > 200)
        {
            preview = preview.substr(0, 200) + "...";
        }
        std::replace(preview.begin(), preview.end(), '\n', ' ');
        printf("%s📋 [%zu chars] %s%s\n", Color::MAGENTA, out.size(), preview.c_str(), Color::RESET);

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
                        JobKey job_key)
{
    while (true)
    {
        // Stream session until done. If idle then no more turns.
        auto streamed = stream_job(client, session_id, job_key, cfg.show_think);
        if (streamed.cancelled)
        {
            return true;
        }
        auto session = std::move(streamed.session);
        if (session.state == SessionState::Idle)
        {
            break;
        }

        // Is the AI waiting for a tools response
        if (session.state.value == SessionState::AwaitingTools)
        {
            // Run tools and send back the result
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
            const auto resp =
                client.post_message(session_id, SessionMessageRequest{.content = *ans, .role = ChatMessage::ROLE_USER});
            job_key = resp.key;
            continue;
        }
    }

    return false;
}

int run_session_mode(RestClient &client, CliConfig &cfg, std::span<const Tool> tools)
{
    install_sigint();
    auto created = client.create_session(
        CreateSessionRequest{.system = cfg.system_prompt, .messages = {}, .questions = cfg.questions});
    const SessionID session_id = created.id;

    printf("%s[client] session %lud at %s (questions=%s)%s\n", Color::CYAN, session_id, client.base_url().c_str(),
           cfg.questions ? "true" : "false", Color::RESET);
    defer(client.delete_session(session_id));

    auto handle_user_text = [&](const string_view &view) -> bool {
        if (view.empty())
        {
            return true;
        }
        try
        {
            const auto resp =
                client.post_message(session_id, SessionMessageRequest{.content = view, .role = ChatMessage::ROLE_USER});
            return drive_session_turn(client, session_id, tools, cfg, resp.key);
        }
        catch (const std::exception &e)
        {
            fprintf(stderr, "%s[client] %s%s\n", Color::RED, e.what(), Color::RESET);
            return false;
        }
    };

    printf("%sType a message, or /help. Ctrl-C cancels generation. Ctrl-D or /exit to quit.%s\n\n", Color::DIM,
           Color::RESET);

    string line;
    while (true)
    {
        g_interrupt.store(false, std::memory_order_relaxed);
        printf("%syou>%s ", Color::BOLD, Color::RESET);
        fflush(stdout);
        if (!std::getline(std::cin, line))
        {
            if (g_interrupt.load(std::memory_order_relaxed))
            {
                std::cin.clear();
                printf("\n");
                continue;
            }
            printf("\n");
            break;
        }

        string_view view(line);

        auto first = view.find_first_not_of(" \t\r\n");
        if (first == string_view::npos)
        {
            continue;
        }
        view = view.substr(first);
        auto last = view.find_last_not_of(" \t\r\n");
        if (last != string_view::npos)
        {
            view = view.substr(0, last + 1);
        }

        if (view == "/exit" || view == "/quit")
        {
            break;
        }
        if (view == "/help")
        {
            print_usage("callisto_client");
            continue;
        }
        if (view == "/state")
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
        if (view == "/tools")
        {
            for (const auto &t : tools)
            {
                printf("  - %s\n", t.name.c_str());
            }
            continue;
        }
        if (view == "/approval")
        {
            cfg.auto_approve = !cfg.auto_approve;
            printf("auto-approve: %s\n", cfg.auto_approve ? "on" : "off");
            continue;
        }
        if (view == "/think")
        {
            cfg.show_think = !cfg.show_think;
            printf("show-think: %s\n", cfg.show_think ? "on" : "off");
            continue;
        }

        handle_user_text(view);

        printf("\n");
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

    printf("%s[client] checking %s ...%s\n", Color::CYAN, client.base_url().c_str(), Color::RESET);
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
        rc = run_session_mode(client, cfg, tools);
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
