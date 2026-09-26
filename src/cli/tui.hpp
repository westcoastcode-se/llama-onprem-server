#pragma once

#include "cli/ui.hpp"

#include <functional>

// Full-screen session. Calls body on a worker thread and returns its status.
int run_tui(const std::function<int(AgentUi &)> &body);
