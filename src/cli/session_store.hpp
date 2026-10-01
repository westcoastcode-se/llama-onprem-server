#pragma once

#include "cli/agent/agent.hpp"

#include <filesystem>
#include <optional>

// Remembers the last session id for this directory and server in ~/.agents.
class SessionStore
{
  public:
    void remember(const AgentConfig &config, SessionID id, const std::filesystem::path &cwd) const;
    [[nodiscard]] std::optional<SessionID> recall(const AgentConfig &config, const std::filesystem::path &cwd) const;

  private:
    [[nodiscard]] std::filesystem::path file() const;
};
