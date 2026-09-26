#include "cli/agent_session.hpp"

#include "cli/reply_stream.hpp"
#include "cli/session_store.hpp"
#include "cli/tool_runner.hpp"
#include "cli/tool_schema.hpp"

#include "api/errors.hpp"
#include "api/models.hpp"
#include "client/rest_client.hpp"

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
                state.ui->note("The message was removed. /compact the session, or send a smaller task.");
                return TurnStatus::ContextFull;
            }
            throw;
        }
        const SessionResponse session = client.get_session(state.session);
        show_context(state, session);
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
            const json queued = client.post_tool_results(state.session, *results);
            key = queued.at("key").get<JobKey>();
            continue;
        }
        if (session.state.value == SessionState::AwaitingQuestion && session.pending_question)
        {
            std::string answer;
            if (state.exec && !isatty(STDIN_FILENO))
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
            key = client.post_message(state.session, message).key;
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
        tools_.push_back(Tools::create_subagent_tool([this](std::string_view task) { return subagent(task); }));
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

std::string AgentSession::subagent(std::string_view task)
{
    RestClient &client = *state_.client;
    const SessionResponse snap = client.snapshot_session(state_.session);
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
        AgentState child_state = state_;
        child_state.session = child;
        child_state.exec = true;
        if (run_turn(child_state, queued.key, base_tools_) != TurnStatus::Idle)
        {
            forget();
            return "sub-agent stopped before it finished";
        }
        message.content = "Summarize the result for the parent. Include files changed and commands run. Do not call tools.";
        queued = client.post_message(child, message);
        if (run_turn(child_state, queued.key, base_tools_) != TurnStatus::Idle)
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
    const std::string instructions = load_ai_instructions(state_.cwd.string());
    request.system = std::format(
        "You are a coding agent working in {}.\n"
        "Use tools to inspect and change the project. Keep edits limited to the task.\n"
        "Do not claim a command or a file change succeeded unless a tool result says so.\n{}",
        state_.cwd.string(), instructions.empty() ? "" : "\nProject instructions:\n" + instructions);
    return request;
}

SessionID AgentSession::open(const AgentConfig &config)
{
    if (config.session != 0)
    {
        state_.client->get_session(config.session);
        return config.session;
    }
    const SessionStore store;
    if (config.resume)
    {
        if (const auto saved = store.recall(config, state_.cwd))
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
    store.remember(config, created.id, state_.cwd);
    return created.id;
}

void AgentSession::refresh_status()
{
    state_.ui->set_status(std::format("session {}   {}   {}   http://{}:{}", state_.session, approval_name(state_.approval),
                                     state_.cwd.string(), config_.host, config_.port));
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
    state_.session = created.id;
    SessionStore{}.remember(config_, created.id, state_.cwd);
    refresh_status();
    state_.ui->note("compacted into session " + std::to_string(created.id));
}

void AgentSession::help() const
{
    state_.ui->note("/help                 show these commands\n"
                    "/approval [mode]      read-only, auto, or full\n"
                    "/status               session id, approval, and server\n"
                    "/diff                 git diff --stat for this directory\n"
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
        state_.ui->note(std::format("session {}\napproval {}\nserver http://{}:{}\ncwd {}", state_.session,
                                   approval_name(state_.approval), config_.host, config_.port, state_.cwd.string()));
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
        AgentConfig fresh = config_;
        fresh.session = 0;
        fresh.resume = false;
        state_.session = open(fresh);
        refresh_status();
        state_.ui->note("session " + std::to_string(state_.session));
        return true;
    }
    if (line == "/compact")
    {
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

int AgentSession::loop()
{
    state_.session = open(config_);
    refresh_status();
    state_.ui->note("Type a task, or /help. Ctrl-C cancels a running turn.");

    if (!config_.prompt.empty())
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
        state_.ui->begin("you");
        state_.ui->append(line);
        state_.ui->end();
        try
        {
            submit(line);
        }
        catch (const std::exception &error)
        {
            state_.ui->note(error.what());
        }
    }
    return 0;
}
