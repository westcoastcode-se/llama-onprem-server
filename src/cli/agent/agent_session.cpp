#include "cli/agent/agent_session.hpp"

#include "cli/agent/context_pressure.hpp"
#include "cli/project/project_context.hpp"
#include "cli/agent/reply_stream.hpp"
#include "cli/session_store.hpp"
#include "cli/agent/tool_runner.hpp"
#include "cli/agent/tool_schema.hpp"
#include "cli/tui/visible_text.hpp"

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

// Keep the tail of the conversation. The head stays so the goal is still visible.
std::string summary_source(std::span<const ChatMessage> messages, int context_size)
{
    std::string transcript;
    for (const ChatMessage &message : messages)
    {
        if (message.role == ChatMessage::ROLE_SYSTEM || message.content.empty())
        {
            continue;
        }
        transcript += message.role;
        transcript += ":\n";
        transcript += message.content;
        transcript += "\n\n";
    }
    return truncate_transcript(transcript, context_size);
}

constexpr std::string_view kSummarizePrompt =
    "Summarize this conversation so a new session can continue the work. Include the goal, decisions, "
    "files changed, commands that mattered, and what is still unfinished. Do not call tools.";

// The server is paused for a client reply. Past 80% the user can compact before that POST.
// A sub-agent compacts on its own. auto_blocked stops a failed attempt from repeating every tool round.
std::optional<TurnStatus> offer_compact(AgentState &state, const SessionResponse &session,
                                        const std::function<TurnStatus(bool)> &compact, bool &auto_blocked)
{
    if (!compact)
    {
        return std::nullopt;
    }
    const bool waiting = server_waiting(session);
    const bool automatic = should_auto_compact(session.context_used, session.context_size, waiting, state.subagent);
    const bool ask = should_offer_compact(session.context_used, session.context_size, waiting, interactive_client(state));
    if (!automatic && !ask)
    {
        return std::nullopt;
    }
    if (automatic && auto_blocked)
    {
        return std::nullopt;
    }
    if (state.ui == nullptr)
    {
        return std::nullopt;
    }
    const int percent = context_percent(session.context_used, session.context_size);
    const SessionID before = state.session;
    TurnStatus status = TurnStatus::Idle;
    if (automatic)
    {
        state.ui->note(std::format("Context is {}%. Compacting and continuing.", percent));
        status = compact(true);
        if (state.session == before)
        {
            auto_blocked = true;
        }
    }
    else
    {
        const auto picked = state.ui->choose(
            std::format("Context is {}%. Compact before the next request?", percent),
            {"Continue", "Compact and continue", "Compact and stop"}, 0);
        if (!picked || *picked == 0)
        {
            return std::nullopt;
        }
        status = compact(*picked == 1);
    }
    if (status == TurnStatus::Cancelled || status == TurnStatus::ContextFull)
    {
        return status;
    }
    if (state.session != before || state.client == nullptr)
    {
        return TurnStatus::Idle;
    }
    try
    {
        const SessionResponse now = state.client->get_session(state.session);
        show_context(state, now);
        if (!server_waiting(now))
        {
            return TurnStatus::Idle;
        }
    }
    catch (const RestClient::ClientError &)
    {
        return TurnStatus::Idle;
    }
    return std::nullopt;
}

TurnStatus run_turn(AgentState &state, JobKey key, std::span<const Tool> tools,
                    const std::function<TurnStatus(bool)> &compact, bool &auto_blocked, bool compacting)
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
                if (compact && !compacting && state.subagent)
                {
                    state.ui->note("Context is full. Compacting and continuing.");
                    return compact(true);
                }
                if (compact && !compacting && interactive_client(state))
                {
                    const auto picked = state.ui->choose("The message was removed because the context is full.",
                                                         {"Stop", "Compact and continue"}, 1);
                    if (picked && *picked == 1)
                    {
                        return compact(true);
                    }
                }
                if (!state.subagent && !compacting)
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
        if (const std::optional<TurnStatus> stopped = offer_compact(state, session, compact, auto_blocked))
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

std::string AgentSession::last_assistant(const std::span<const ChatMessage> messages)
{
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
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
    AgentState child_state = state_;
    child_state.subagent = true;
    child_state.session = 0;
    if (inherit)
    {
        child_state.session = client.snapshot_session(state_.session).id;
    }
    else
    {
        CreateSessionRequest request = make_request(config_);
        request.tools.reserve(base_tools_.size());
        for (const Tool &tool : base_tools_)
        {
            request.tools.push_back(ToolSchema::chat_tool(tool));
        }
        child_state.session = client.create_session(request).id;
    }
    auto forget = [&] {
        if (child_state.session == 0 || child_state.session == state_.session)
        {
            return;
        }
        try
        {
            client.delete_session(child_state.session);
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
        auto queued = client.post_message(child_state.session, message);
        const SubagentLive live(state_.ui);
        auto_compact_blocked_ = false;
        const std::function<TurnStatus(bool)> compact_child = [this, &child_state](bool resume) {
            return compact_session(child_state, base_tools_, resume, false);
        };
        const TurnStatus work =
            run_turn(child_state, queued.key, base_tools_, compact_child, auto_compact_blocked_, false);
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
        queued = client.post_message(child_state.session, message);
        bool ignored = false;
        const TurnStatus summary_turn = run_turn(child_state, queued.key, base_tools_, {}, ignored, false);
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
        std::string summary = last_assistant(client.get_messages(child_state.session));
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
    auto_compact_blocked_ = false;
    std::function<TurnStatus(bool)> compact;
    if (!compacting_)
    {
        compact = [this](bool resume) { return this->compact(resume); };
    }
    return run_turn(state_, key, tools_, compact, auto_compact_blocked_, compacting_);
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
    extra += "\nLook at the files in the project root and determine what kind of project this is before working in an "
             "unfamiliar area.\n"
             "A sub_agent can gather that information when the question needs more than the root listing. "
             "Its result should describe what the question needs: the kind of project, how it is built and tested, "
             "and the paths that matter. Use that description to carry out the question.\n";
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
    const SessionStore store;
    if (config.session != 0)
    {
        present_transcript_of(stored, config.session);
        return config.session;
    }
    if (config.resume)
    {
        if (const auto saved = store.recall(stored, state_.cwd))
        {
            present_transcript_of(stored, *saved);
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

void AgentSession::present_transcript_of(const AgentConfig &stored, SessionID id)
{
    if (!SessionStore{}.owns(stored, id))
    {
        throw std::runtime_error("session was not created on this computer");
    }
    CreateSessionRequest request;
    request.id = id;
    const SessionResponse resumed = state_.client->create_session(request);
    if (resumed.id != id)
    {
        throw std::runtime_error("server resumed a different session");
    }
    SessionStore{}.remember(stored, id, state_.cwd);
    present_transcript(resumed, resumed.messages);
}

void AgentSession::present_transcript(const SessionResponse &header, const std::span<const ChatMessage> messages) const
{
    if (state_.ui == nullptr)
    {
        return;
    }
    state_.ui->clear();
    if (!header.system_prompt.empty())
    {
        state_.ui->show_system(header.system_prompt);
    }
    for (const ChatMessage &message : messages)
    {
        if (message.content.empty() && message.reasoning_content.empty())
        {
            continue;
        }
        if (message.role == ChatMessage::ROLE_ASSISTANT)
        {
            auto shown = [&](std::string_view text) {
                if (state_.debug)
                {
                    return std::string(text);
                }
                VisibleText filter;
                std::string out = filter.feed(text);
                out += filter.finish();
                return out;
            };
            if (state_.show_think)
            {
                std::string thinking = shown(message.reasoning_content);
                if (!thinking.empty())
                {
                    state_.ui->show_saved_thinking(std::move(thinking));
                }
            }
            std::string visible = shown(message.content);
            if (!visible.empty())
            {
                state_.ui->begin("assistant");
                state_.ui->append(std::move(visible));
                state_.ui->end();
            }
            continue;
        }
        if (message.role == ChatMessage::ROLE_SYSTEM)
        {
            state_.ui->show_system(message.content);
            continue;
        }
        const bool tool = message.content.find("<tool_response>") != std::string::npos ||
                          message.content.find("</tool_response>") != std::string::npos;
        state_.ui->begin(tool ? "tool" : "you");
        state_.ui->append(message.content);
        state_.ui->end();
    }
    if (header.state.value == SessionState::AwaitingTools)
    {
        state_.ui->note("this session is waiting on tool results");
    }
    else if (header.state.value == SessionState::AwaitingQuestion && header.pending_question)
    {
        state_.ui->note("this session is waiting on a question: " + header.pending_question->text);
    }
    else if (header.state.value == SessionState::Generating)
    {
        state_.ui->note("this session is still generating");
    }
}

void AgentSession::resume_session()
{
    if (state_.client == nullptr || !state_.client->probe())
    {
        state_.ui->note(offline_message());
        return;
    }
    const AgentConfig stored = endpoint_config(config_);
    const SessionStore store;
    const std::vector<SessionResponse> sessions = state_.client->list_sessions();
    std::vector<SessionResponse> mine;
    mine.reserve(sessions.size());
    for (const SessionResponse &session : sessions)
    {
        if (store.owns(stored, session.id))
        {
            mine.push_back(session);
        }
    }
    if (mine.empty())
    {
        state_.ui->note("no sessions created on this computer");
        return;
    }
    std::vector<std::string> rows;
    std::size_t selected = 0;
    rows.reserve(mine.size());
    for (std::size_t i = 0; i < mine.size(); ++i)
    {
        const SessionResponse &session = mine[i];
        if (session.id == state_.session)
        {
            selected = i;
        }
        rows.push_back(std::format("{}  {}  {}/{} tok", session.id, session.state.to_string(), session.context_used,
                                   session.context_size));
    }
    const std::optional<std::size_t> picked = state_.ui->choose("Resume session", std::move(rows), selected);
    if (!picked || *picked >= mine.size())
    {
        return;
    }
    const SessionID id = mine[*picked].id;
    present_transcript_of(stored, id);
    bind_session(id);
    refresh_status();
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
    return compact_session(state_, tools_, resume, true);
}

std::pair<TurnStatus, std::string> AgentSession::summarize_for_compact(AgentState &state, std::span<const Tool> tools)
{
    SessionMessageRequest message;
    message.role = "user";
    message.content = std::string(kSummarizePrompt);
    const auto queued = state.client->post_message(state.session, message);
    bool blocked = false;
    const TurnStatus status = run_turn(state, queued.key, tools, {}, blocked, true);
    if (status == TurnStatus::Idle)
    {
        return {TurnStatus::Idle, last_assistant(state.client->get_messages(state.session))};
    }
    if (status != TurnStatus::ContextFull)
    {
        return {status, {}};
    }

    // The summarize prompt did not fit in this session. Retry from a shortened transcript.
    int window = 0;
    std::string transcript;
    try
    {
        window = state.client->get_session(state.session).context_size;
        transcript = summary_source(state.client->get_messages(state.session), window);
    }
    catch (const RestClient::ClientError &)
    {
        return {TurnStatus::ContextFull, {}};
    }
    if (transcript.empty())
    {
        return {TurnStatus::ContextFull, {}};
    }

    CreateSessionRequest request = make_request(config_);
    const SessionResponse scratch = state.client->create_session(request);
    struct Forget
    {
        RestClient *client = nullptr;
        SessionID id = 0;
        ~Forget()
        {
            if (client == nullptr || id == 0)
            {
                return;
            }
            try
            {
                client->delete_session(id);
            }
            catch (...)
            {
            }
        }
    } forget{state.client, scratch.id};

    message.content = std::format("{}\n\nConversation:\n{}", kSummarizePrompt, transcript);
    const auto scratch_queued = state.client->post_message(scratch.id, message);
    AgentState scratch_state = state;
    scratch_state.session = scratch.id;
    const TurnStatus scratch_status = run_turn(scratch_state, scratch_queued.key, tools, {}, blocked, true);
    if (scratch_status != TurnStatus::Idle)
    {
        const TurnStatus failed = scratch_status == TurnStatus::Cancelled ? TurnStatus::Cancelled : TurnStatus::ContextFull;
        return {failed, {}};
    }
    return {TurnStatus::Idle, last_assistant(state.client->get_messages(scratch.id))};
}

TurnStatus AgentSession::compact_session(AgentState &state, std::span<const Tool> tools, bool resume, bool install)
{
    if (compacting_ || state.client == nullptr || state.ui == nullptr)
    {
        return TurnStatus::Idle;
    }
    if (compact_depth_ >= 3)
    {
        return TurnStatus::ContextFull;
    }
    ++compact_depth_;
    struct Depth
    {
        int &n;
        ~Depth()
        {
            --n;
        }
    } depth{compact_depth_};

    const SessionID source = state.session;
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

        const auto [status, summary] = summarize_for_compact(state, tools);
        if (status == TurnStatus::Cancelled || status == TurnStatus::ContextFull)
        {
            return status;
        }
        if (summary.empty())
        {
            state.ui->note("compact produced no summary");
            return TurnStatus::Idle;
        }

        CreateSessionRequest request = make_request(config_);
        request.tools.reserve(tools.size());
        for (const Tool &tool : tools)
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
        const SessionResponse created = state.client->create_session(request);
        if (install)
        {
            bind_session(created.id);
            SessionStore{}.remember(endpoint_config(config_), created.id, state_.cwd);
            present_system(created);
            refresh_status();
            state.ui->note("compacted into session " + std::to_string(created.id));
        }
        else
        {
            state.session = created.id;
            if (source != 0 && source != state_.session && source != created.id)
            {
                try
                {
                    state.client->delete_session(source);
                }
                catch (...)
                {
                }
            }
            state.ui->note("sub-agent compacted into session " + std::to_string(created.id));
        }
    }

    if (!resume)
    {
        return TurnStatus::Idle;
    }
    constexpr std::string_view kContinue = "Continue the unfinished work from the summary. Use tools.";
    if (install)
    {
        state.ui->begin("you");
        state.ui->append(std::string(kContinue));
        state.ui->end();
        return submit(std::string(kContinue));
    }
    state.ui->note("Continuing the sub-agent from the summary.");
    SessionMessageRequest message;
    message.role = "user";
    message.content = std::string(kContinue);
    const auto queued = state.client->post_message(state.session, message);
    const std::function<TurnStatus(bool)> again = [this, &state, tools](bool resume_again) {
        return compact_session(state, tools, resume_again, false);
    };
    return run_turn(state, queued.key, tools, again, auto_compact_blocked_, false);
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
