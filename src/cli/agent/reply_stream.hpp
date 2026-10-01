#pragma once

#include "cli/agent/agent_state.hpp"

#include "api/sessions.hpp"

// Reads one generation from the server and paints thinking and assistant text.
class ReplyStream
{
  public:
    explicit ReplyStream(AgentState &state) : state_(state)
    {
    }

    void read(JobKey key);

  private:
    AgentState &state_;
};
