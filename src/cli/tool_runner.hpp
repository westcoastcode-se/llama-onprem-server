#pragma once

#include "cli/agent_state.hpp"

#include "common/std.hpp"
#include "common/tools.hpp"

#include <optional>
#include <span>

// Runs the tool calls the server is waiting on, including the approval prompt.
class ToolRunner
{
  public:
    explicit ToolRunner(AgentState &state) : state_(state)
    {
    }

    [[nodiscard]] std::optional<json> run(const SessionResponse &session, std::span<const Tool> tools);

  private:
    AgentState &state_;
};
