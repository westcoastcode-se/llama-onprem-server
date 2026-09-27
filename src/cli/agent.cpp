#include "cli/agent.hpp"
#include "cli/agent_session.hpp"
#include "cli/agent_state.hpp"
#include "cli/console_ui.hpp"
#include "cli/servers.hpp"
#include "cli/theme.hpp"
#include "cli/tui.hpp"
#include "cli/ui.hpp"

#include "client/rest_client.hpp"
#include "common/log.hpp"
#include "common/tools.hpp"

#include <csignal>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{

void on_interrupt(int)
{
    g_agent_interrupt.store(true, std::memory_order_relaxed);
}

} // namespace

int run_agent(const AgentConfig &incoming)
{
    std::signal(SIGINT, on_interrupt);
    Logger::set_level(Logger::LEVEL_ERROR);

    std::vector<ServerTarget> targets = incoming.servers;
    if (targets.empty())
    {
        ServerTarget only;
        only.host = incoming.host;
        only.port = incoming.port;
        targets.push_back(std::move(only));
    }

    std::vector<std::unique_ptr<RestClient>> clients;
    std::vector<ServerSlot> slots;
    clients.reserve(targets.size());
    slots.reserve(targets.size());
    for (const ServerTarget &target : targets)
    {
        auto client = std::make_unique<RestClient>(target.host, target.port);
        ServerSlot slot;
        slot.target = target;
        slot.client = client.get();
        slots.push_back(slot);
        clients.push_back(std::move(client));
    }

    const std::optional<std::size_t> chosen = first_reachable(slots.size(), [&](std::size_t index) {
        return slots[index].client->probe();
    });

    AgentConfig config = incoming;
    if (chosen)
    {
        config.host = slots[*chosen].target.host;
        config.port = slots[*chosen].target.port;
    }
    config.servers = targets;

    AgentState state;
    state.servers = std::move(slots);
    state.active = chosen.value_or(0);
    state.client = state.servers[state.active].client;
    state.awaiting_server = !chosen;
    state.approval = config.approval;
    state.show_think = config.show_think;
    state.debug = config.debug;
    state.exec = config.exec;

    const Theme theme = load_theme(config.theme);
    AgentSession session(state, config, get_registered_tools(false, nullptr));
    if (config.exec)
    {
        ConsoleUi ui(theme);
        state.ui = &ui;
        return session.loop();
    }
    return run_tui(theme, [&](AgentUi &ui) {
        state.ui = &ui;
        return session.loop();
    });
}
