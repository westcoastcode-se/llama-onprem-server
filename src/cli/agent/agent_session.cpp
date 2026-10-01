#include "cli/agent/agent_session.hpp"

#include "cli/agent/context_pressure.hpp"
#include "cli/project/project_context.hpp"
#include "cli/agent/reply_stream.hpp"
#include "cli/session_store.hpp"
#include "cli/agent/tool_runner.hpp"
#include "cli/agent/tool_schema.hpp"

#include "api/errors.hpp"
#include "api/models.hpp"
#include "cli/rest_client.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <optional>
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

bool interactive_client(const AgentState &state)
{
    if (state.ui == nullptr || state.subagent)
    {
        return false;
    }
    if (state.exec && !isatty(STDIN_FILENO))
    {
        return false;
    }
    return true;
}

bool server_waiting(const SessionResponse &session)
{
    if (session.state.value == SessionState::AwaitingTools)
    {
        return true;
    }
    return session.state.value == SessionState::AwaitingQuestion && session.pending_question.has_value();
}

// The server is paused for a client reply. Past 80% the user can compact before that POST.
std::optional<TurnStatus> offer_compact(AgentState &state, const SessionResponse &session,
                                        const std::function<TurnStatus(bool)> &compact)
{
    if (!compact || !should_offer_compact(session.context_used, session.context_size, server_waiting(session),
                                          interactive_client(state)))
    {
        return std::nullopt;
    }
    const auto picked = state.ui->choose(
        std::format("Context is {}%. Compact before the next request?",
                    context_percent(session.context_used, session.context_size)),
        {"Continue", "Compact and continue", "Compact and stop"}, 0);
    if (!picked || *picked == 0)
    {
        return std::nullopt;
    }
    const SessionID before = state.session;
    const TurnStatus status = compact(*picked == 1);
    if (status == TurnStatus::Cancelled || status == TurnStatus::ContextFull)
    {
        return status;
    }
    if (state.session != before || state.client == nullptr)
    {
        return TurnStatus::Idle;
    }
    const SessionResponse now = state.client->get_session(state.session);
    show_context(state, now);
    if (!server_waiting(now))
    {
        return TurnStatus::Idle;
    }
    return std::nullopt;
}

TurnStatus run_turn(AgentState &state, JobKey key, std::span<const Tool> tools,
                    const std::function<TurnStatus(bool)> &compact = {})
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
        if (const std::optional<TurnStatus> stopped = offer_compact(state, session, compact))
        {
            return *stopped;
        }
        if (session.state.value == SessionState::AwaitingTools)
        {
            auto results = ToolRunner{state}.run(session, tools);
            if (!results)
            {
                return TurnStatus::Cancelled;
            }
            const SessionMessageResponse queued = client.post_tool_results(session_id, *results);
            key = queued.key;
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
        message +=
            " The copied conversation filled the window. Leave inherit false unless the task needs that history.";
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
                          "Include files changed and commands run. Do not paste file contents. Do not call tools.";
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
    std::function<TurnStatus(bool)> compact;
    if (!compacting_)
    {
        compact = [this](bool resume) { return this->compact(resume); };
    }
    return run_turn(state_, key, tools_, compact);
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
    request.compress_tools = config.compress_tools;
    std::string extra;
    const std::string instructions = load_agents_markdown(state_.cwd.string());
    if (!instructions.empty())
    {
        extra += "\nProject instructions:\n" + instructions + "\n";
    }
    extra += "\nIf .agents/map.md exists, read it with read_file before searching an unfamiliar area. "
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
    request.system = std::format("You are a coding agent working in {}.\n"
                                 "Use tools to inspect and change the project. Keep edits limited to the task.\n"
                                 "Build and change programs one piece at a time. Do not think out or write the whole "
                                 "solution in one turn.\n"
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
    present_system(created);
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
    present_system(created);
    return created.id;
}

void AgentSession::present_system(const SessionResponse &created) const
{
    if (created.system_prompt.empty() || state_.ui == nullptr)
    {
        return;
    }
    state_.ui->show_system(created.system_prompt);
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
    const std::string model =
        state_.servers.empty() ? std::format("{}:{}", config_.host, config_.port) : server.label();
    state_.ui->set_status(std::format("{}   session {}   {}   {}", model, state_.session,
                                      approval_name(state_.approval), state_.cwd.string()));
    show_context(state_, state_.client->get_session(state_.session));
}

TurnStatus AgentSession::compact(bool resume)
{
    if (compacting_)
    {
        return TurnStatus::Idle;
    }
    std::string summary;
    {
        compacting_ = true;
        struct Clear
        {
            bool &flag;
            ~Clear()
            {
                flag = false;
            }
        } clear{compacting_};

        const TurnStatus status =
            submit("Summarize this conversation so a new session can continue the work. Include the goal, decisions, "
                   "files changed, commands that mattered, and what is still unfinished. Do not call tools.");
        if (status != TurnStatus::Idle)
        {
            return status;
        }
        summary = last_assistant(state_.client->get_session(state_.session));
        if (summary.empty())
        {
            state_.ui->note("compact produced no summary");
            return TurnStatus::Idle;
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
        present_system(created);
        refresh_status();
        state_.ui->note("compacted into session " + std::to_string(created.id));
    }

    if (!resume)
    {
        return TurnStatus::Idle;
    }
    constexpr std::string_view kContinue = "Continue the unfinished work from the summary. Use tools.";
    state_.ui->begin("you");
    state_.ui->append(std::string(kContinue));
    state_.ui->end();
    return submit(std::string(kContinue));
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
            state_.ui->note("wrote .agents/map.md");
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
