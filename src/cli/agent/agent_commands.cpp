#include "cli/agent/agent_session.hpp"

#include "cli/project/project_context.hpp"
#include "cli/servers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

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

} // namespace

void AgentSession::help() const
{
    state_.ui->note("/help                 show these commands\n"
                    "/model [name]         choose a server, or switch by name\n"
                    "/approval [mode]      read-only, auto, or full\n"
                    "/status               session id, approval, and server\n"
                    "/diff                 git diff --stat for this directory\n"
                    "/map                  write .agents/map.md from the tree\n"
                    "/compact              summarize the chat into a new session and stop\n"
                    "/clear                start a new session\n"
                    "/exit                 leave\n"
                    "Ctrl-C cancels the current generation. A running command is aborted, and the server is told how long it ran.\n"
                    "Click a thinking or tool line, or the system box, or press Ctrl-O, to open or close it.\n"
                    "When context is over 80% and the server is waiting, you can compact before the next request.\n"
                    "Compact and continue keeps working from the summary. Compact and stop leaves you at the prompt.");
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
        const std::string model =
            state_.servers.empty() ? std::format("{}:{}", config_.host, config_.port) : server.label();
        const std::string url =
            state_.servers.empty() ? std::format("http://{}:{}", config_.host, config_.port) : server.url();
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
                state_.ui->note("wrote .agents/map.md");
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
        (void)compact(false);
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
        if (same_label(state_.servers[i].target.label(), argument) ||
            same_label(state_.servers[i].target.url(), argument) ||
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

