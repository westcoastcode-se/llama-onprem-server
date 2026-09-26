#pragma once

#include "cli/agent_state.hpp"

#include "common/tools.hpp"

#include <string>
#include <string_view>
#include <vector>

// One interactive or exec session: open, slash commands, and the tool loop.
class AgentSession
{
  public:
    AgentSession(AgentState &state, const AgentConfig &config, std::vector<Tool> base_tools);
    AgentSession(const AgentSession &) = delete;
    AgentSession &operator=(const AgentSession &) = delete;

    [[nodiscard]] int loop();

  private:
    AgentState &state_;
    const AgentConfig &config_;
    std::vector<Tool> base_tools_;
    std::vector<Tool> tools_;

    [[nodiscard]] std::string subagent(std::string_view task);
    [[nodiscard]] TurnStatus drive(JobKey key);
    TurnStatus submit(const std::string &prompt);
    [[nodiscard]] SessionID open(const AgentConfig &config);
    [[nodiscard]] CreateSessionRequest make_request(const AgentConfig &config) const;
    [[nodiscard]] static std::string last_assistant(const SessionResponse &session);
    void refresh_status();
    void compact();
    void help() const;
    void diff() const;
    [[nodiscard]] bool slash(const std::string &line);
};
