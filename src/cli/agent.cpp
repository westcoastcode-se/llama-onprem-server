#include "cli/agent.hpp"
#include "cli/agent_session.hpp"
#include "cli/agent_state.hpp"
#include "cli/console_ui.hpp"
#include "cli/tui.hpp"
#include "cli/ui.hpp"

#include "client/rest_client.hpp"
#include "common/log.hpp"
#include "common/tools.hpp"

#include <csignal>
#include <format>
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

int run_agent(const AgentConfig &config)
{
    std::signal(SIGINT, on_interrupt);
    Logger::set_level(Logger::LEVEL_ERROR);
    RestClient client(config.host, config.port);
    if (!client.health())
    {
        throw std::runtime_error(std::format("server not reachable at {}", client.base_url()));
    }

    AgentState state;
    state.client = &client;
    state.approval = config.approval;
    state.show_think = config.show_think;
    state.debug = config.debug;
    state.exec = config.exec;

    AgentSession session(state, config, get_registered_tools(false, nullptr));
    if (config.exec)
    {
        ConsoleUi ui;
        state.ui = &ui;
        return session.loop();
    }
    return run_tui([&](AgentUi &ui) {
        state.ui = &ui;
        return session.loop();
    });
}
