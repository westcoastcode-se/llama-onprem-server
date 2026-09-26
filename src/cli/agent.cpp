#include "cli/agent.hpp"

#include "api/errors.hpp"
#include "api/models.hpp"
#include "client/rest_client.hpp"
#include "common/color.hpp"
#include "common/log.hpp"
#include "common/std.hpp"
#include "common/tools.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <utility>

namespace
{

std::atomic<bool> g_interrupt{false};

void on_interrupt(int)
{
    g_interrupt.store(true, std::memory_order_relaxed);
}

enum class TurnStatus
{
    Idle,
    Cancelled,
    ContextFull
};

struct AgentState
{
    RestClient *client = nullptr;
    SessionID session = 0;
    ApprovalMode approval = ApprovalMode::ReadOnly;
    bool show_think = true;
    bool exec = false;
    int max_rounds = 40;
    std::vector<std::string> always_tools;
    std::filesystem::path cwd = std::filesystem::current_path();
};

const char *approval_name(ApprovalMode mode)
{
    switch (mode)
    {
    case ApprovalMode::ReadOnly:
        return "read-only";
    case ApprovalMode::Auto:
        return "auto";
    case ApprovalMode::Full:
        return "full";
    }
    return "read-only";
}

enum class ToolKind
{
    Read,
    Write,
    Shell,
    Network,
    Other
};

ToolKind tool_kind(std::string_view name)
{
    if (name == "read_file" || name == "list_directory" || name == "file_search" || name == "search_text")
    {
        return ToolKind::Read;
    }
    if (name == "write_file")
    {
        return ToolKind::Write;
    }
    if (name == "execute_command")
    {
        return ToolKind::Shell;
    }
    if (name == "web_fetch" || name == "web_search")
    {
        return ToolKind::Network;
    }
    return ToolKind::Other;
}

bool path_in_workspace(const std::filesystem::path &raw, const std::filesystem::path &cwd)
{
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(cwd, ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::path target = raw;
    if (target.empty())
    {
        return false;
    }
    if (!target.is_absolute())
    {
        target = cwd / target;
    }
    const auto canon = std::filesystem::weakly_canonical(target, ec);
    if (ec)
    {
        return false;
    }
    const auto rel = std::filesystem::relative(canon, root, ec);
    if (ec)
    {
        return false;
    }
    const auto text = rel.generic_string();
    return text != ".." && !text.starts_with("../");
}

std::string arg_text(const json &args, const char *key)
{
    if (!args.is_object() || !args.contains(key) || args[key].is_null())
    {
        return {};
    }
    if (args[key].is_string())
    {
        return args[key].get<std::string>();
    }
    return args[key].dump();
}

bool tool_is_always(const AgentState &state, std::string_view name)
{
    return is_tool_allowed(name, false, state.always_tools);
}

bool needs_approval(const AgentState &state, std::string_view name, const json &args)
{
    if (state.approval == ApprovalMode::Full || tool_is_always(state, name))
    {
        return false;
    }
    switch (tool_kind(name))
    {
    case ToolKind::Read:
        return false;
    case ToolKind::Write:
        if (state.approval == ApprovalMode::Auto && path_in_workspace(arg_text(args, "path"), state.cwd))
        {
            return false;
        }
        return true;
    case ToolKind::Shell:
    case ToolKind::Network:
    case ToolKind::Other:
        return true;
    }
    return true;
}

void print_edit_preview(const std::filesystem::path &cwd, const json &args)
{
    const std::string path_text = arg_text(args, "path");
    const std::string content = arg_text(args, "content");
    std::filesystem::path path = path_text;
    if (!path.is_absolute())
    {
        path = cwd / path;
    }
    std::string previous;
    {
        std::ifstream in(path);
        if (in)
        {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            previous = buffer.str();
        }
    }
    std::println(stderr, "{}--- {}{}", Color::DIM, path.string(), Color::RESET);
    std::println(stderr, "{}+++ {} bytes{}", Color::DIM, content.size(), Color::RESET);
    std::istringstream old_lines(previous);
    std::istringstream new_lines(content);
    std::string old_line;
    std::string new_line;
    bool have_old = static_cast<bool>(std::getline(old_lines, old_line));
    bool have_new = static_cast<bool>(std::getline(new_lines, new_line));
    int shown = 0;
    while ((have_old || have_new) && shown < 40)
    {
        if (have_old && have_new && old_line == new_line)
        {
            have_old = static_cast<bool>(std::getline(old_lines, old_line));
            have_new = static_cast<bool>(std::getline(new_lines, new_line));
            continue;
        }
        if (have_old)
        {
            std::println(stderr, "{}- {}{}", Color::RED, old_line, Color::RESET);
            have_old = static_cast<bool>(std::getline(old_lines, old_line));
            ++shown;
        }
        if (have_new && shown < 40)
        {
            std::println(stderr, "{}+ {}{}", Color::GREEN, new_line, Color::RESET);
            have_new = static_cast<bool>(std::getline(new_lines, new_line));
            ++shown;
        }
    }
}

void print_tool_header(std::string_view name, const json &args)
{
    std::string brief;
    if (name == "execute_command")
    {
        brief = arg_text(args, "command");
    }
    else if (name == "write_file" || name == "read_file" || name == "list_directory" || name == "file_search" ||
             name == "search_text")
    {
        brief = arg_text(args, "path");
        if (brief.empty())
        {
            brief = arg_text(args, "query");
        }
    }
    else if (name == "web_fetch")
    {
        brief = arg_text(args, "url");
    }
    else if (name == "web_search")
    {
        brief = arg_text(args, "query");
    }
    else if (name == "sub_agent")
    {
        brief = arg_text(args, "task");
    }
    if (brief.size() > 120)
    {
        brief.resize(117);
        brief += "...";
    }
    std::println(stderr, "{}• {} {}{}", Color::CYAN, name, brief, Color::RESET);
}

enum class Ask
{
    Once,
    AlwaysTool,
    Full,
    Deny,
    Closed
};

Ask ask_approval(std::string_view name, const json &args, const AgentState &state)
{
    if (state.exec && !isatty(STDIN_FILENO))
    {
        std::println(stderr, "{}denied {} (exec has no terminal to approve it){}", Color::YELLOW, name, Color::RESET);
        return Ask::Deny;
    }
    if (name == "write_file")
    {
        print_edit_preview(state.cwd, args);
    }
    else if (name == "execute_command")
    {
        std::println(stderr, "{}{}{}", Color::YELLOW, arg_text(args, "command"), Color::RESET);
    }
    std::print(stderr, "Allow {}? [y]es / [n]o / [a]lways this tool / [f]ull access: ", name);
    std::fflush(stderr);
    std::string line;
    if (!std::getline(std::cin, line))
    {
        return Ask::Closed;
    }
    if (line.empty() || line == "y" || line == "yes")
    {
        return Ask::Once;
    }
    if (line == "a" || line == "always")
    {
        return Ask::AlwaysTool;
    }
    if (line == "f" || line == "full")
    {
        return Ask::Full;
    }
    return Ask::Deny;
}

std::optional<json> run_tools(AgentState &state, const SessionResponse &session, const std::vector<Tool> &tools)
{
    json results = json::array();
    for (const ParsedToolCall &call : session.pending_tool_calls)
    {
        json args = call.arguments.is_object() ? call.arguments : json::object();
        print_tool_header(call.name, args);
        json item{{"id", call.id}, {"name", call.name}, {"denied", false}, {"content", ""}};
        if (needs_approval(state, call.name, args))
        {
            switch (ask_approval(call.name, args, state))
            {
            case Ask::Once:
                break;
            case Ask::AlwaysTool:
                state.always_tools.emplace_back(call.name);
                break;
            case Ask::Full:
                state.approval = ApprovalMode::Full;
                std::println(stderr, "{}approval is now full{}", Color::GREEN, Color::RESET);
                break;
            case Ask::Deny:
                item["denied"] = true;
                item["content"] = "denied by user";
                results.push_back(std::move(item));
                continue;
            case Ask::Closed:
                return std::nullopt;
            }
        }
        else if (tool_kind(call.name) == ToolKind::Write)
        {
            print_edit_preview(state.cwd, args);
        }
        const std::string output = run_tool(tools, call.name, args);
        item["content"] = output;
        std::string preview = output;
        if (preview.size() > 160)
        {
            preview.resize(157);
            preview += "...";
        }
        std::ranges::replace(preview, '\n', ' ');
        std::println(stderr, "{}  {}{}", Color::DIM, preview, Color::RESET);
        results.push_back(std::move(item));
    }
    return results;
}

std::string_view trim_copy_line(std::string_view text)
{
    constexpr std::string_view ws = " \t\r";
    const auto begin = text.find_first_not_of(ws);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto end = text.find_last_not_of(ws);
    return text.substr(begin, end - begin + 1);
}

json parameters_schema(std::string_view schema_doc)
{
    json properties = json::object();
    json required = json::array();
    std::size_t pos = 0;
    while (pos < schema_doc.size())
    {
        const std::size_t nl = schema_doc.find('\n', pos);
        const std::string_view line =
            trim_copy_line(schema_doc.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? schema_doc.size() : nl + 1;
        if (line.empty() || line.starts_with("arguments"))
        {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
        {
            continue;
        }
        const std::string_view name = trim_copy_line(line.substr(0, colon));
        if (name.empty() || name.find(' ') != std::string_view::npos)
        {
            continue;
        }
        const std::string_view rest = trim_copy_line(line.substr(colon + 1));
        const std::size_t word_end = rest.find_first_of(" \t");
        const std::string_view type_word = rest.substr(0, word_end);
        std::string type = "string";
        if (type_word.starts_with("bool"))
        {
            type = "boolean";
        }
        else if (type_word.starts_with("int"))
        {
            type = "integer";
        }
        else if (type_word.starts_with("number"))
        {
            type = "number";
        }
        else if (type_word.starts_with("array"))
        {
            type = "array";
        }
        else if (type_word.starts_with("object"))
        {
            type = "object";
        }
        json spec = {{"type", type}};
        const std::size_t open = rest.find('(');
        const std::size_t close = rest.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open)
        {
            spec["description"] = std::string(trim_copy_line(rest.substr(open + 1, close - open - 1)));
        }
        properties[std::string(name)] = std::move(spec);
        if (rest.find("optional") == std::string_view::npos)
        {
            required.push_back(std::string(name));
        }
    }
    json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
    {
        schema["required"] = std::move(required);
    }
    return schema;
}

ChatTool to_chat_tool(const Tool &tool)
{
    ChatTool spec;
    spec.name = tool.name;
    spec.description = tool.description;
    const json schema = parameters_schema(tool.schema_doc);
    if (schema["properties"].empty() && !tool.schema_doc.empty())
    {
        spec.description.push_back('\n');
        spec.description += tool.schema_doc;
    }
    spec.parameters = schema.dump();
    return spec;
}

void stream_reply(RestClient &client, SessionID session_id, JobKey key, bool show_think)
{
    std::string pending;
    bool thinking = false;
    bool header = false;
    auto emit = [](std::string_view text) { std::print("{}", text); };
    auto take_think = [&](std::string_view text) {
        if (!show_think)
        {
            return;
        }
        while (!header && !text.empty() && (text.front() == '\n' || text.front() == '\r'))
        {
            text.remove_prefix(1);
        }
        if (text.empty())
        {
            return;
        }
        if (!header)
        {
            std::println(stderr, "{}thinking{}", Color::DIM, Color::RESET);
            std::print("{}", Color::GRAY);
            header = true;
        }
        emit(text);
    };

    std::jthread watcher([&](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            if (g_interrupt.load(std::memory_order_relaxed))
            {
                client.stop();
                try
                {
                    RestClient killer(client.host(), client.port());
                    killer.cancel_job(session_id, key);
                }
                catch (...)
                {
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    try
    {
        client.stream_tokens(session_id, key, [&](const string &piece) {
            if (g_interrupt.load(std::memory_order_relaxed))
            {
                return false;
            }
            pending += piece;
            while (!pending.empty())
            {
                if (!thinking)
                {
                    const auto open = pending.find("<think>");
                    if (open == std::string::npos)
                    {
                        if (pending.size() > 7)
                        {
                            emit(std::string_view(pending).substr(0, pending.size() - 7));
                            pending.erase(0, pending.size() - 7);
                        }
                        break;
                    }
                    emit(std::string_view(pending).substr(0, open));
                    pending.erase(0, open + std::string("<think>").size());
                    thinking = true;
                    continue;
                }
                const auto close = pending.find("</think>");
                if (close == std::string::npos)
                {
                    if (pending.size() > 8)
                    {
                        take_think(std::string_view(pending).substr(0, pending.size() - 8));
                        pending.erase(0, pending.size() - 8);
                    }
                    break;
                }
                take_think(std::string_view(pending).substr(0, close));
                pending.erase(0, close + std::string("</think>").size());
                thinking = false;
                if (header)
                {
                    std::println("{}", Color::RESET);
                    header = false;
                }
            }
            return true;
        });
    }
    catch (...)
    {
        watcher.request_stop();
        watcher.join();
        if (header)
        {
            std::println("{}", Color::RESET);
        }
        std::println("");
        throw;
    }
    watcher.request_stop();
    watcher.join();
    if (!thinking)
    {
        emit(pending);
    }
    else if (show_think)
    {
        take_think(pending);
    }
    if (header)
    {
        std::println("{}", Color::RESET);
    }
    std::println("");
}

std::filesystem::path session_file()
{
    const char *home = std::getenv("HOME");
    std::filesystem::path dir = home != nullptr ? std::filesystem::path(home) / ".callisto" : std::filesystem::path(".callisto");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "last-session";
}

void remember_session(const AgentConfig &config, SessionID id, const std::filesystem::path &cwd)
{
    std::ofstream out(session_file());
    if (!out)
    {
        return;
    }
    out << config.host << '\n' << config.port << '\n' << cwd.string() << '\n' << id << '\n';
}

std::optional<SessionID> recalled_session(const AgentConfig &config, const std::filesystem::path &cwd)
{
    std::ifstream in(session_file());
    if (!in)
    {
        return std::nullopt;
    }
    std::string host;
    std::string port_text;
    std::string saved_cwd;
    std::string id_text;
    if (!std::getline(in, host) || !std::getline(in, port_text) || !std::getline(in, saved_cwd) ||
        !std::getline(in, id_text))
    {
        return std::nullopt;
    }
    try
    {
        if (host != config.host || std::stoi(port_text) != config.port || saved_cwd != cwd.string())
        {
            return std::nullopt;
        }
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
    if (id_text.empty() || id_text.find_first_not_of("0123456789") != std::string::npos)
    {
        return std::nullopt;
    }
    return static_cast<SessionID>(std::stoull(id_text));
}

std::string last_assistant(const SessionResponse &session)
{
    for (auto it = session.messages.rbegin(); it != session.messages.rend(); ++it)
    {
        if (it->role == ChatMessage::ROLE_ASSISTANT && !it->content.empty())
        {
            return it->content;
        }
    }
    return {};
}

TurnStatus drive(AgentState &state, JobKey key, const std::vector<Tool> &tools);

std::string run_subagent(AgentState &parent, std::string_view task, const std::vector<Tool> &base_tools)
{
    RestClient &client = *parent.client;
    const SessionResponse snap = client.snapshot_session(parent.session);
    const SessionID child = snap.id;
    auto forget = [&] {
        try
        {
            client.delete_session(child);
        }
        catch (...)
        {
        }
    };
    try
    {
        const std::string assignment = std::format(
            "You are a sub-agent. Work only on the task below and do not start another sub-agent.\n\nTask:\n{}", task);
        SessionMessageRequest message;
        message.role = "user";
        message.content = assignment;
        auto queued = client.post_message(child, message);
        AgentState child_state = parent;
        child_state.session = child;
        child_state.exec = true;
        if (drive(child_state, queued.key, base_tools) != TurnStatus::Idle)
        {
            forget();
            return "sub-agent stopped before it finished";
        }
        message.content = "Summarize the result for the parent. Include files changed and commands run. Do not call tools.";
        queued = client.post_message(child, message);
        if (drive(child_state, queued.key, base_tools) != TurnStatus::Idle)
        {
            forget();
            return "sub-agent stopped before the summary";
        }
        const std::string summary = last_assistant(client.get_session(child));
        forget();
        return summary.empty() ? "sub-agent produced no summary" : summary;
    }
    catch (...)
    {
        forget();
        throw;
    }
}

TurnStatus drive(AgentState &state, JobKey key, const std::vector<Tool> &tools)
{
    RestClient &client = *state.client;
    for (int round = 0; round < state.max_rounds; ++round)
    {
        if (g_interrupt.exchange(false, std::memory_order_relaxed))
        {
            client.cancel_job(state.session, key);
            std::println(stderr, "{}cancelled{}", Color::YELLOW, Color::RESET);
            return TurnStatus::Cancelled;
        }
        try
        {
            stream_reply(client, state.session, key, state.show_think);
        }
        catch (const RestClient::ClientError &error)
        {
            if (g_interrupt.exchange(false, std::memory_order_relaxed))
            {
                std::println(stderr, "{}cancelled{}", Color::YELLOW, Color::RESET);
                return TurnStatus::Cancelled;
            }
            if (error.code == kContextFull)
            {
                std::println(stderr, "{}", error.what());
                std::println(stderr, "The message was removed. /compact the session, or send a smaller task.");
                return TurnStatus::ContextFull;
            }
            throw;
        }
        const SessionResponse session = client.get_session(state.session);
        if (session.state.value == SessionState::Idle || session.state.value == SessionState::Unknown)
        {
            return TurnStatus::Idle;
        }
        if (session.state.value == SessionState::AwaitingTools)
        {
            auto results = run_tools(state, session, tools);
            if (!results)
            {
                return TurnStatus::Cancelled;
            }
            const json queued = client.post_tool_results(state.session, *results);
            key = queued.at("key").get<JobKey>();
            continue;
        }
        if (session.state.value == SessionState::AwaitingQuestion && session.pending_question)
        {
            std::println(stderr, "{}? {}{}", Color::YELLOW, session.pending_question->text, Color::RESET);
            for (std::size_t i = 0; i < session.pending_question->answers.size(); ++i)
            {
                std::println(stderr, "  {} {}", i, session.pending_question->answers[i]);
            }
            std::string answer;
            if (state.exec && !isatty(STDIN_FILENO))
            {
                answer = "Continue with the most reasonable choice.";
            }
            else
            {
                std::print(stderr, "answer> ");
                std::fflush(stderr);
                if (!std::getline(std::cin, answer) || answer.empty())
                {
                    return TurnStatus::Cancelled;
                }
                const bool digits =
                    std::ranges::all_of(answer, [](unsigned char c) { return std::isdigit(c) != 0; });
                if (digits && !session.pending_question->answers.empty())
                {
                    const int index = std::stoi(answer);
                    if (index >= 0 && static_cast<std::size_t>(index) < session.pending_question->answers.size())
                    {
                        answer = session.pending_question->answers[static_cast<std::size_t>(index)];
                    }
                }
            }
            SessionMessageRequest message;
            message.role = "user";
            message.content = std::move(answer);
            key = client.post_message(state.session, message).key;
            continue;
        }
        return TurnStatus::Idle;
    }
    std::println(stderr, "{}stopped after {} tool rounds{}", Color::YELLOW, state.max_rounds, Color::RESET);
    return TurnStatus::Idle;
}

void print_help()
{
    std::println(stderr, "  /help                 show these commands");
    std::println(stderr, "  /approval [mode]      read-only, auto, or full");
    std::println(stderr, "  /status               session id, approval, and server");
    std::println(stderr, "  /diff                 git diff --stat for this directory");
    std::println(stderr, "  /compact              summarize the chat into a new session");
    std::println(stderr, "  /clear                start a new session");
    std::println(stderr, "  /exit                 leave");
    std::println(stderr, "Ctrl-C cancels the current generation.");
}

void print_diff(const std::filesystem::path &)
{
    FILE *pipe = popen("git diff --stat", "r");
    if (pipe == nullptr)
    {
        std::println(stderr, "git diff failed to start");
        return;
    }
    char buffer[512];
    std::string output;
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    pclose(pipe);
    if (output.empty())
    {
        std::println(stderr, "no unstaged diff");
        return;
    }
    std::print(stderr, "{}", output);
}

CreateSessionRequest make_session_request(const AgentConfig &config, const std::filesystem::path &cwd)
{
    CreateSessionRequest request;
    request.questions = config.questions;
    const std::string instructions = load_ai_instructions(cwd.string());
    request.system = std::format(
        "You are a coding agent working in {}.\n"
        "Use tools to inspect and change the project. Keep edits limited to the task.\n"
        "Do not claim a command or a file change succeeded unless a tool result says so.\n{}",
        cwd.string(), instructions.empty() ? "" : "\nProject instructions:\n" + instructions);
    return request;
}

SessionID open_session(AgentState &state, const AgentConfig &config, const std::vector<Tool> &tools)
{
    if (config.session != 0)
    {
        state.client->get_session(config.session);
        return config.session;
    }
    if (config.resume)
    {
        if (const auto saved = recalled_session(config, state.cwd))
        {
            state.client->get_session(*saved);
            return *saved;
        }
        throw std::runtime_error("no saved session for this directory and server");
    }
    CreateSessionRequest request = make_session_request(config, state.cwd);
    request.tools.reserve(tools.size());
    for (const Tool &tool : tools)
    {
        request.tools.push_back(to_chat_tool(tool));
    }
    const SessionResponse created = state.client->create_session(request);
    remember_session(config, created.id, state.cwd);
    return created.id;
}

TurnStatus submit_prompt(AgentState &state, const std::string &prompt, const std::vector<Tool> &tools)
{
    SessionMessageRequest message;
    message.role = "user";
    message.content = prompt;
    const auto queued = state.client->post_message(state.session, message);
    return drive(state, queued.key, tools);
}

void compact_session(AgentState &state, const AgentConfig &config, const std::vector<Tool> &tools)
{
    const TurnStatus status = submit_prompt(
        state,
        "Summarize this conversation so a new session can continue the work. Include the goal, decisions, "
        "files changed, commands that mattered, and what is still unfinished. Do not call tools.",
        tools);
    if (status != TurnStatus::Idle)
    {
        return;
    }
    const std::string summary = last_assistant(state.client->get_session(state.session));
    if (summary.empty())
    {
        std::println(stderr, "compact produced no summary");
        return;
    }
    CreateSessionRequest request = make_session_request(config, state.cwd);
    for (const Tool &tool : tools)
    {
        request.tools.push_back(to_chat_tool(tool));
    }
    ChatMessage prior;
    prior.role = "user";
    prior.content = "Conversation so far:\n" + summary;
    ChatMessage ack;
    ack.role = "assistant";
    ack.content = "I'll continue from that summary.";
    request.messages = {std::move(prior), std::move(ack)};
    const SessionResponse created = state.client->create_session(request);
    state.session = created.id;
    remember_session(config, created.id, state.cwd);
    std::println(stderr, "{}compacted into session {}{}", Color::GREEN, created.id, Color::RESET);
}

bool slash_command(const std::string &line, AgentState &state, const AgentConfig &config, const std::vector<Tool> &tools)
{
    if (line == "/help")
    {
        print_help();
        return true;
    }
    if (line == "/status")
    {
        std::println(stderr, "session {}", state.session);
        std::println(stderr, "approval {}", approval_name(state.approval));
        std::println(stderr, "server http://{}:{}", config.host, config.port);
        std::println(stderr, "cwd {}", state.cwd.string());
        return true;
    }
    if (line == "/diff")
    {
        print_diff(state.cwd);
        return true;
    }
    if (line == "/exit" || line == "/quit")
    {
        return false;
    }
    if (line == "/clear")
    {
        state.session = 0;
        AgentConfig fresh = config;
        fresh.session = 0;
        fresh.resume = false;
        state.session = open_session(state, fresh, tools);
        std::println(stderr, "session {}", state.session);
        return true;
    }
    if (line == "/compact")
    {
        compact_session(state, config, tools);
        return true;
    }
    if (line.starts_with("/approval"))
    {
        const auto space = line.find(' ');
        if (space == std::string::npos)
        {
            std::println(stderr, "approval {}", approval_name(state.approval));
            return true;
        }
        const std::string mode = line.substr(space + 1);
        if (mode == "read-only" || mode == "suggest")
        {
            state.approval = ApprovalMode::ReadOnly;
        }
        else if (mode == "auto")
        {
            state.approval = ApprovalMode::Auto;
        }
        else if (mode == "full")
        {
            state.approval = ApprovalMode::Full;
        }
        else
        {
            std::println(stderr, "approval is read-only, auto, or full");
            return true;
        }
        std::println(stderr, "approval {}", approval_name(state.approval));
        return true;
    }
    if (line.starts_with('/'))
    {
        std::println(stderr, "unknown command. /help lists them.");
        return true;
    }
    return false;
}

} // namespace

int run_agent(const AgentConfig &config)
{
    std::signal(SIGINT, on_interrupt);
    Logger::set_level(Logger::LEVEL_ERROR);
    RestClient client(config.host, config.port);
    if (!client.health())
    {
        throw std::runtime_error(std::format("server not reachable at {}", client.base_url()));
    }

    const std::vector<Tool> base_tools = get_registered_tools(false, nullptr);
    AgentState state;
    state.client = &client;
    state.approval = config.approval;
    state.show_think = config.show_think;
    state.exec = config.exec;

    std::vector<Tool> tools = base_tools;
    if (!config.exec)
    {
        tools.push_back(Tools::create_subagent_tool([&](std::string_view task) {
            return run_subagent(state, task, base_tools);
        }));
    }

    state.session = open_session(state, config, tools);
    std::println(stderr, "{}callisto{}  session {}  approval {}  {}", Color::BOLD, Color::RESET, state.session,
                 approval_name(state.approval), state.cwd.string());
    std::println(stderr, "server {}", client.base_url());

    if (!config.prompt.empty())
    {
        const TurnStatus status = submit_prompt(state, config.prompt, tools);
        if (status == TurnStatus::ContextFull)
        {
            return 2;
        }
        if (config.exec)
        {
            return status == TurnStatus::Cancelled ? 1 : 0;
        }
    }
    else if (config.exec)
    {
        throw std::runtime_error("exec needs a task");
    }

    if (config.exec)
    {
        return 0;
    }

    std::println(stderr, "Type a task, or /help. Ctrl-C cancels a running turn. Ctrl-D exits.");
    std::string line;
    while (true)
    {
        g_interrupt.store(false, std::memory_order_relaxed);
        std::print(stderr, "{}you>{} ", Color::BOLD, Color::RESET);
        std::fflush(stderr);
        if (!std::getline(std::cin, line))
        {
            std::println(stderr, "");
            break;
        }
        if (line == "/exit" || line == "/quit")
        {
            break;
        }
        if (line.empty())
        {
            continue;
        }
        bool handled = false;
        try
        {
            handled = slash_command(line, state, config, tools);
        }
        catch (const std::exception &error)
        {
            std::println(stderr, "{}", error.what());
            continue;
        }
        if (!handled && (line.starts_with('/')))
        {
            continue;
        }
        if (line == "/exit" || line == "/quit")
        {
            break;
        }
        if (handled)
        {
            if (line == "/exit" || line == "/quit")
            {
                break;
            }
            continue;
        }
        try
        {
            if (submit_prompt(state, line, tools) == TurnStatus::ContextFull)
            {
                continue;
            }
        }
        catch (const std::exception &error)
        {
            std::println(stderr, "{}", error.what());
        }
    }
    return 0;
}
