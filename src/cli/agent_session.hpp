#pragma once

#include "cli/agent_state.hpp"

#include "common/tools.hpp"

#include <optional>
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
    [[nodiscard]] SessionID open_with_history(std::vector<ChatMessage> history);
    [[nodiscard]] CreateSessionRequest make_request(const AgentConfig &config) const;
    [[nodiscard]] AgentConfig endpoint_config(const AgentConfig &config) const;
    void bind_session(SessionID id);
    [[nodiscard]] ServerSlot &slot();
    [[nodiscard]] const ServerSlot &slot() const;
    [[nodiscard]] std::optional<std::size_t> find_model(std::string_view query) const;
    void list_models();
    void use_model(std::size_t index);
    [[nodiscard]] bool model_command(std::string_view argument);
    [[nodiscard]] std::string offline_message() const;
    [[nodiscard]] bool ensure_server();
    [[nodiscard]] static std::string last_assistant(const SessionResponse &session);
    void refresh_status();
    void compact();
    void help() const;
    void diff() const;
    [[nodiscard]] bool slash(const std::string &line);
};
