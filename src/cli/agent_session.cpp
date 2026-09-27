#include "cli/agent_session.hpp"

#include "cli/project_context.hpp"
#include "cli/reply_stream.hpp"
#include "cli/session_store.hpp"
#include "cli/tool_runner.hpp"
#include "cli/tool_schema.hpp"

#include "api/errors.hpp"
#include "api/models.hpp"
#include "client/rest_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace
{

struct PipeClose
{
    void operator()(FILE *pipe) const
    {
        if (pipe != nullptr)
        {
            pclose(pipe);
        }
    }
};

using Pipe = std::unique_ptr<FILE, PipeClose>;

struct SubagentLive
{
    AgentUi *ui = nullptr;

    explicit SubagentLive(AgentUi *ui) : ui(ui)
    {
        ui->set_subagent_live(true);
    }

    ~SubagentLive()
    {
        ui->set_subagent_live(false);
    }
};

std::string git_diff_stat()
{
    Pipe pipe(popen("git diff --stat", "r"));
    if (pipe == nullptr)
    {
        return {};
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
    {
        output += buffer;
        if (output.size() > 4000)
        {
            output.resize(4000);
            output += "\n... [diff stat truncated]\n";
            break;
        }
    }
    return output;
}

bool same_label(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b))
        {
            return false;
        }
    }
    return true;
}

TurnStatus run_turn(AgentState &state, JobKey key, std::span<const Tool> tools)
{
    RestClient &client = *state.client;
    for (int round = 0; round < state.max_rounds; ++round)
    {
        if (g_agent_interrupt.exchange(false, std::memory_order_relaxed))
        {
            client.cancel_job(state.session, key);
            state.ui->note("cancelled");
            return TurnStatus::Cancelled;
        }
        try
        {
            ReplyStream{state}.read(key);
        }
        catch (const RestClient::ClientError &error)
        {
            if (g_agent_interrupt.exchange(false, std::memory_order_relaxed))
            {
                state.ui->note("cancelled");
                return TurnStatus::Cancelled;
            }
            if (error.code == kContextFull)
            {
                state.ui->note(error.what());
                if (!state.subagent)
                {
                    state.ui->note("The message was removed. /compact the session, or send a smaller task.");
                }
                return TurnStatus::ContextFull;
            }
            throw;
        }
        const SessionResponse session = client.get_session(state.session);
        show_context(state, session);
        // Captured before tools run, so a nested run cannot retarget this POST.
        const SessionID session_id = state.session;
        if (session.state.value == SessionState::Idle || session.state.value == SessionState::Unknown)
        {
            return TurnStatus::Idle;
        }
        if (session.state.value == SessionState::AwaitingTools)
        {
            auto results = ToolRunner{state}.run(session, tools);
            if (!results)
            {
                return TurnStatus::Cancelled;
            }
            const json queued = client.post_tool_results(session_id, *results);
            key = queued.at("key").get<JobKey>();
            continue;
        }
        if (session.state.value == SessionState::AwaitingQuestion && session.pending_question)
        {
            std::string answer;
            if (state.subagent || (state.exec && !isatty(STDIN_FILENO)))
            {
                answer = "Continue with the most reasonable choice.";
            }
            else
            {
                auto typed = state.ui->question(session.pending_question->text, session.pending_question->answers);
                if (!typed || typed->empty())
                {
                    return TurnStatus::Cancelled;
                }
                answer = std::move(*typed);
            }
            SessionMessageRequest message;
            message.role = "user";
            message.content = std::move(answer);
            key = client.post_message(session_id, message).key;
            continue;
        }
        return TurnStatus::Idle;
    }
    state.ui->note("stopped after " + std::to_string(state.max_rounds) + " tool rounds");
    return TurnStatus::Idle;
}

} // namespace

AgentSession::AgentSession(AgentState &state, const AgentConfig &config, std::vector<Tool> base_tools)
    : state_(state), config_(config), base_tools_(std::move(base_tools)), tools_(base_tools_)
{
    if (!config_.exec)
    {
        tools_.push_back(Tools::create_subagent_tool(
            [this](std::string_view task, bool inherit) { return subagent(task, inherit); }));
    }
}

std::string AgentSession::last_assistant(const SessionResponse &session)
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

std::string context_full_reply(bool inherit)
{
    std::string message = "error: sub-agent ran out of context. Call sub_agent again with a smaller task so the answer "
                          "can be shorter. Do not repeat the same question.";
    if (inherit)
    {
        message += " The copied conversation filled the window. Leave inherit false unless the task needs that history.";
    }
    return message;
}

std::string AgentSession::subagent(std::string_view task, bool inherit)
{
    RestClient &client = *state_.client;
    SessionID child = 0;
    if (inherit)
    {
        child = client.snapshot_session(state_.session).id;
    }
    else
    {
        CreateSessionRequest request = make_request(config_);
        request.tools.reserve(base_tools_.size());
        for (const Tool &tool : base_tools_)
        {
            request.tools.push_back(ToolSchema::chat_tool(tool));
        }
        child = client.create_session(request).id;
    }
    auto forget = [&] {
        if (child == 0 || child == state_.session)
        {
            return;
        }
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
        AgentState child_state = state_;
        child_state.session = child;
        child_state.subagent = true;
        const SubagentLive live(state_.ui);
        const TurnStatus work = run_turn(child_state, queued.key, base_tools_);
        if (work == TurnStatus::ContextFull)
        {
            forget();
            return context_full_reply(inherit);
        }
        if (work != TurnStatus::Idle)
        {
            forget();
            return "sub-agent stopped before it finished";
        }
        message.content = "Summarize the result for the parent. Cite file:line for each finding. "
                          "Include files changed and commands run. Do not call tools.";
        queued = client.post_message(child, message);
        const TurnStatus summary_turn = run_turn(child_state, queued.key, base_tools_);
        if (summary_turn == TurnStatus::ContextFull)
        {
            forget();
            return context_full_reply(inherit);
        }
        if (summary_turn != TurnStatus::Idle)
        {
            forget();
            return "sub-agent stopped before the summary";
        }
        std::string summary = last_assistant(client.get_session(child));
        if (summary.empty())
        {
            summary = "sub-agent produced no summary";
        }
        const std::string stat = git_diff_stat();
        if (!stat.empty())
        {
            summary += "\n\nWorking tree:\n" + stat;
        }
        forget();
        return summary;
    }
    catch (...)
    {
        forget();
        throw;
    }
}

TurnStatus AgentSession::drive(JobKey key)
{
    return run_turn(state_, key, tools_);
}

TurnStatus AgentSession::submit(const std::string &prompt)
{
    SessionMessageRequest message;
    message.role = "user";
    message.content = prompt;
    const auto queued = state_.client->post_message(state_.session, message);
    return drive(queued.key);
}

CreateSessionRequest AgentSession::make_request(const AgentConfig &config) const
{
    CreateSessionRequest request;
    request.questions = config.questions;
    std::string extra;
    const std::string instructions = load_ai_instructions(state_.cwd.string());
    if (!instructions.empty())
    {
        extra += "\nProject instructions:\n" + instructions + "\n";
    }
    extra += "\nIf .callisto/map.md exists, read it with read_file before searching an unfamiliar area. "
             "It is an index, not the source.\n";
    const std::vector<SkillNote> skills = list_skills(state_.cwd);
    if (!skills.empty())
    {
        extra += "Skills. Read the matching SKILL.md with read_file only when the task needs that procedure:\n";
        for (const SkillNote &skill : skills)
        {
            extra += std::format("- {}: {} ({})\n", skill.name, skill.summary, skill.path);
        }
    }
    request.system = std::format(
        "You are a coding agent working in {}.\n"
        "Use tools to inspect and change the project. Keep edits limited to the task.\n"
        "Do not claim a command or a file change succeeded unless a tool result says so.\n{}",
        state_.cwd.string(), extra);
    return request;
}

AgentConfig AgentSession::endpoint_config(const AgentConfig &config) const
{
    AgentConfig stored = config;
    if (!state_.servers.empty())
    {
        stored.host = slot().target.host;
        stored.port = slot().target.port;
    }
    return stored;
}

void AgentSession::bind_session(SessionID id)
{
    state_.session = id;
    if (!state_.servers.empty())
    {
        slot().session = id;
    }
}

ServerSlot &AgentSession::slot()
{
    return state_.servers.at(state_.active);
}

const ServerSlot &AgentSession::slot() const
{
    return state_.servers.at(state_.active);
}

SessionID AgentSession::open(const AgentConfig &config)
{
    const AgentConfig stored = endpoint_config(config);
    if (config.session != 0)
    {
        state_.client->get_session(config.session);
        return config.session;
    }
    const SessionStore store;
    if (config.resume)
    {
        if (const auto saved = store.recall(stored, state_.cwd))
        {
            state_.client->get_session(*saved);
            return *saved;
        }
        throw std::runtime_error("no saved session for this directory and server");
    }
    CreateSessionRequest request = make_request(config);
    request.tools.reserve(tools_.size());
    for (const Tool &tool : tools_)
    {
        request.tools.push_back(ToolSchema::chat_tool(tool));
    }
    const SessionResponse created = state_.client->create_session(request);
    store.remember(stored, created.id, state_.cwd);
    return created.id;
}

SessionID AgentSession::open_with_history(std::vector<ChatMessage> history)
{
    AgentConfig fresh = config_;
    fresh.session = 0;
    fresh.resume = false;
    CreateSessionRequest request = make_request(fresh);
    request.tools.reserve(tools_.size());
    for (const Tool &tool : tools_)
    {
        request.tools.push_back(ToolSchema::chat_tool(tool));
    }
    request.messages = std::move(history);
    const SessionResponse created = state_.client->create_session(request);
    SessionStore{}.remember(endpoint_config(fresh), created.id, state_.cwd);
    return created.id;
}

void AgentSession::refresh_status()
{
    if (state_.session == 0 || state_.client == nullptr)
    {
        state_.ui->set_status(std::format("offline   {}   {}", approval_name(state_.approval), state_.cwd.string()));
        state_.ui->set_context(0, 0);
        return;
    }
    const ServerTarget &server = state_.servers.empty() ? ServerTarget{} : slot().target;
    const std::string model = state_.servers.empty() ? std::format("{}:{}", config_.host, config_.port) : server.label();
    state_.ui->set_status(std::format("{}   session {}   {}   {}", model, state_.session, approval_name(state_.approval),
                                     state_.cwd.string()));
    show_context(state_, state_.client->get_session(state_.session));
}

void AgentSession::compact()
{
    const TurnStatus status = submit(
        "Summarize this conversation so a new session can continue the work. Include the goal, decisions, "
        "files changed, commands that mattered, and what is still unfinished. Do not call tools.");
    if (status != TurnStatus::Idle)
    {
        return;
    }
    const std::string summary = last_assistant(state_.client->get_session(state_.session));
    if (summary.empty())
    {
        state_.ui->note("compact produced no summary");
        return;
    }
    CreateSessionRequest request = make_request(config_);
    for (const Tool &tool : tools_)
    {
        request.tools.push_back(ToolSchema::chat_tool(tool));
    }
    ChatMessage prior;
    prior.role = "user";
    prior.content = "Conversation so far:\n" + summary;
    ChatMessage ack;
    ack.role = "assistant";
    ack.content = "I'll continue from that summary.";
    request.messages = {std::move(prior), std::move(ack)};
    const SessionResponse created = state_.client->create_session(request);
    bind_session(created.id);
    SessionStore{}.remember(endpoint_config(config_), created.id, state_.cwd);
    refresh_status();
    state_.ui->note("compacted into session " + std::to_string(created.id));
}

void AgentSession::help() const
{
    state_.ui->note("/help                 show these commands\n"
                    "/model [name]         choose a server, or switch by name\n"
                    "/approval [mode]      read-only, auto, or full\n"
                    "/status               session id, approval, and server\n"
                    "/diff                 git diff --stat for this directory\n"
                    "/map                  write .callisto/map.md from the tree\n"
                    "/compact              summarize the chat into a new session\n"
                    "/clear                start a new session\n"
                    "/exit                 leave\n"
                    "Ctrl-C cancels the current generation.\n"
                    "Click a thinking or tool line, or press Ctrl-O, to open or close it.");
}

void AgentSession::diff() const
{
    Pipe pipe(popen("git diff --stat", "r"));
    if (pipe == nullptr)
    {
        state_.ui->note("git diff failed to start");
        return;
    }
    char buffer[512];
    std::string output;
    while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr)
    {
        output += buffer;
    }
    if (output.empty())
    {
        state_.ui->note("no unstaged diff");
        return;
    }
    state_.ui->note(output);
}

bool AgentSession::slash(const std::string &line)
{
    if (line == "/help")
    {
        help();
        return true;
    }
    if (line == "/status")
    {
        if (state_.session == 0 || state_.client == nullptr)
        {
            state_.ui->note(std::format("{}\napproval {}\ncwd {}", offline_message(), approval_name(state_.approval),
                                       state_.cwd.string()));
            return true;
        }
        const ServerTarget &server = state_.servers.empty() ? ServerTarget{} : slot().target;
        const std::string model = state_.servers.empty() ? std::format("{}:{}", config_.host, config_.port) : server.label();
        const std::string url = state_.servers.empty() ? std::format("http://{}:{}", config_.host, config_.port) : server.url();
        state_.ui->note(std::format("model {}\nserver {}\nsession {}\napproval {}\ncwd {}", model, url, state_.session,
                                   approval_name(state_.approval), state_.cwd.string()));
        return true;
    }
    if (line == "/diff")
    {
        diff();
        return true;
    }
    if (line == "/exit" || line == "/quit")
    {
        return false;
    }
    if (line == "/clear")
    {
        if (!ensure_server())
        {
            return true;
        }
        AgentConfig fresh = config_;
        fresh.session = 0;
        fresh.resume = false;
        bind_session(open(fresh));
        refresh_status();
        state_.ui->note("session " + std::to_string(state_.session));
        return true;
    }
    if (line == "/model" || line.starts_with("/model "))
    {
        std::string_view argument(line);
        argument.remove_prefix(std::string_view("/model").size());
        return model_command(string_view_trim(argument));
    }
    if (line == "/map")
    {
        try
        {
            if (refresh_project_map(state_.cwd, true))
            {
                state_.ui->note("wrote .callisto/map.md");
            }
        }
        catch (const std::exception &error)
        {
            state_.ui->note(error.what());
        }
        return true;
    }
    if (line == "/compact")
    {
        if (state_.session == 0)
        {
            state_.ui->note(offline_message());
            return true;
        }
        compact();
        refresh_status();
        return true;
    }
    if (line.starts_with("/approval"))
    {
        const auto space = line.find(' ');
        if (space == std::string::npos)
        {
            state_.ui->note(std::string("approval ") + approval_name(state_.approval));
            return true;
        }
        const std::string mode = line.substr(space + 1);
        if (mode == "read-only" || mode == "suggest")
        {
            state_.approval = ApprovalMode::ReadOnly;
        }
        else if (mode == "auto")
        {
            state_.approval = ApprovalMode::Auto;
        }
        else if (mode == "full")
        {
            state_.approval = ApprovalMode::Full;
        }
        else
        {
            state_.ui->note("approval is read-only, auto, or full");
            return true;
        }
        refresh_status();
        state_.ui->note(std::string("approval ") + approval_name(state_.approval));
        return true;
    }
    if (line.starts_with('/'))
    {
        state_.ui->note("unknown command. /help lists them.");
        return true;
    }
    return false;
}

std::optional<std::size_t> AgentSession::find_model(std::string_view query) const
{
    if (query.empty() || state_.servers.empty())
    {
        return std::nullopt;
    }
    const bool digits = std::ranges::all_of(query, [](unsigned char c) { return std::isdigit(c) != 0; });
    if (digits)
    {
        try
        {
            const int number = std::stoi(std::string(query));
            if (number >= 1 && static_cast<std::size_t>(number) <= state_.servers.size())
            {
                return static_cast<std::size_t>(number - 1);
            }
        }
        catch (const std::exception &)
        {
        }
        return std::nullopt;
    }
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < state_.servers.size(); ++i)
    {
        const ServerTarget &server = state_.servers[i].target;
        const bool match = same_label(server.label(), query) || same_label(server.url(), query) ||
                           same_label(std::format("{}:{}", server.host, server.port), query);
        if (!match)
        {
            continue;
        }
        if (found)
        {
            return std::nullopt;
        }
        found = i;
    }
    return found;
}

void AgentSession::list_models()
{
    if (state_.servers.empty())
    {
        state_.ui->note("no models");
        return;
    }
    std::vector<std::string> rows;
    std::size_t selected = 0;
    rows.reserve(state_.servers.size());
    for (std::size_t i = 0; i < state_.servers.size(); ++i)
    {
        ServerSlot &server = state_.servers[i];
        const bool up = server.client != nullptr && server.client->probe();
        if (state_.session != 0 && i == state_.active)
        {
            selected = i;
        }
        rows.push_back(std::format("{}  {}{}", server.target.label(), server.target.url(), up ? "" : "  down"));
    }
    const std::optional<std::size_t> picked = state_.ui->choose("Model", std::move(rows), selected);
    if (!picked)
    {
        return;
    }
    use_model(*picked);
}

void AgentSession::use_model(std::size_t index)
{
    if (index >= state_.servers.size() || state_.servers[index].client == nullptr)
    {
        state_.ui->note("unknown model. /model lists them.");
        return;
    }
    ServerSlot &next = state_.servers[index];
    if (!next.client->probe())
    {
        state_.ui->note(std::format("{} is not reachable at {}", next.target.label(), next.target.url()));
        return;
    }
    if (index == state_.active && state_.session != 0)
    {
        state_.ui->note(std::format("already using {}", next.target.label()));
        return;
    }
    const std::size_t previous = state_.active;
    RestClient *previous_client = state_.client;
    const SessionID previous_session = state_.session;
    std::vector<ChatMessage> history;
    if (previous_session != 0 && previous_client != nullptr)
    {
        history = previous_client->get_session(previous_session).messages;
    }
    const std::size_t carried = history.size();
    state_.active = index;
    state_.client = next.client;
    try
    {
        next.session = open_with_history(std::move(history));
        bind_session(next.session);
    }
    catch (...)
    {
        state_.active = previous;
        state_.client = previous_client;
        state_.session = previous_session;
        if (index < state_.servers.size())
        {
            state_.servers[index].session = 0;
        }
        throw;
    }
    refresh_status();
    if (carried == 0)
    {
        state_.ui->note(std::format("using {}  {}", next.target.label(), next.target.url()));
    }
    else
    {
        state_.ui->note(std::format("using {}  {}  ({} messages)", next.target.label(), next.target.url(), carried));
    }
}

std::string AgentSession::offline_message() const
{
    std::string message = "no server is reachable";
    for (const ServerSlot &server : state_.servers)
    {
        message += std::format("\n  {}  {}", server.target.label(), server.target.url());
    }
    return message;
}

bool AgentSession::ensure_server()
{
    if (state_.session != 0 && state_.client != nullptr && state_.client->probe())
    {
        state_.awaiting_server = false;
        return true;
    }
    const std::optional<std::size_t> chosen = first_reachable(state_.servers.size(), [&](std::size_t index) {
        RestClient *client = state_.servers[index].client;
        return client != nullptr && client->probe();
    });
    if (!chosen)
    {
        state_.awaiting_server = true;
        state_.ui->note(offline_message());
        refresh_status();
        return false;
    }
    if (*chosen == state_.active && state_.session != 0)
    {
        state_.awaiting_server = false;
        return true;
    }
    use_model(*chosen);
    state_.awaiting_server = state_.session == 0;
    return state_.session != 0;
}

bool AgentSession::model_command(std::string_view argument)
{
    if (argument.empty())
    {
        list_models();
        return true;
    }
    std::size_t matches = 0;
    if (const bool digits = std::ranges::all_of(argument, [](unsigned char c) { return std::isdigit(c) != 0; }); digits)
    {
        const std::optional<std::size_t> index = find_model(argument);
        if (!index)
        {
            state_.ui->note("unknown model. /model lists them.");
            return true;
        }
        use_model(*index);
        return true;
    }
    for (std::size_t i = 0; i < state_.servers.size(); ++i)
    {
        if (same_label(state_.servers[i].target.label(), argument) || same_label(state_.servers[i].target.url(), argument) ||
            same_label(std::format("{}:{}", state_.servers[i].target.host, state_.servers[i].target.port), argument))
        {
            ++matches;
        }
    }
    if (matches > 1)
    {
        state_.ui->note("that name is used by more than one server. /model lists them; use the number.");
        return true;
    }
    const std::optional<std::size_t> index = find_model(argument);
    if (!index)
    {
        state_.ui->note("unknown model. /model lists them.");
        return true;
    }
    use_model(*index);
    return true;
}

int AgentSession::loop()
{
    if (!state_.awaiting_server)
    {
        try
        {
            bind_session(open(config_));
            refresh_status();
            state_.ui->note(std::format("using {}  {}", slot().target.label(), slot().target.url()));
        }
        catch (const std::exception &error)
        {
            state_.awaiting_server = true;
            state_.session = 0;
            if (!state_.servers.empty())
            {
                slot().session = 0;
            }
            state_.ui->note(error.what());
            refresh_status();
        }
    }
    else if (!config_.exec)
    {
        state_.ui->note(offline_message());
        refresh_status();
    }
    try
    {
        if (refresh_project_map(state_.cwd, false))
        {
            state_.ui->note("wrote .callisto/map.md");
        }
    }
    catch (const std::exception &error)
    {
        state_.ui->note(error.what());
    }
    if (!config_.exec)
    {
        state_.ui->note("Type a task, or /help. Ctrl-C cancels a running turn.");
    }

    if (!config_.prompt.empty())
    {
        if (!ensure_server())
        {
            if (config_.exec)
            {
                return 1;
            }
        }
        else
        {
            state_.ui->begin("you");
            state_.ui->append(config_.prompt);
            state_.ui->end();
            const TurnStatus status = submit(config_.prompt);
            if (status == TurnStatus::ContextFull)
            {
                return 2;
            }
            if (config_.exec)
            {
                return status == TurnStatus::Cancelled ? 1 : 0;
            }
        }
    }
    else if (config_.exec)
    {
        throw std::runtime_error("exec needs a task");
    }
    if (config_.exec)
    {
        return 0;
    }

    while (true)
    {
        g_agent_interrupt.store(false, std::memory_order_relaxed);
        const std::string line = state_.ui->read_line();
        if (line.empty())
        {
            break;
        }
        if (line == "/exit" || line == "/quit")
        {
            break;
        }
        bool handled = false;
        try
        {
            handled = slash(line);
        }
        catch (const std::exception &error)
        {
            state_.ui->note(error.what());
            continue;
        }
        if (handled)
        {
            continue;
        }
        try
        {
            if (!ensure_server())
            {
                continue;
            }
            state_.ui->begin("you");
            state_.ui->append(line);
            state_.ui->end();
            submit(line);
        }
        catch (const std::exception &error)
        {
            state_.ui->note(error.what());
        }
    }
    return 0;
}
