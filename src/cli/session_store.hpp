#pragma once

#include "cli/agent/agent.hpp"

#include <filesystem>
#include <optional>

// Sessions created on this computer, under ~/.agents.
// last-session is the latest id for one directory and server.
// sessions lists every id created here, so resume can ignore the rest.
class SessionStore
{
  public:
    void remember(const AgentConfig &config, SessionID id, const std::filesystem::path &cwd) const;

    // Adds id to the set created on this computer. Does not change the directory's last session.
    void record(const AgentConfig &config, SessionID id) const;

    [[nodiscard]] std::optional<SessionID> recall(const AgentConfig &config, const std::filesystem::path &cwd) const;

    // True when this computer created id against config's host and port.
    [[nodiscard]] bool owns(const AgentConfig &config, SessionID id) const;

  private:
    [[nodiscard]] std::filesystem::path file() const;
    [[nodiscard]] std::filesystem::path registry() const;
};
