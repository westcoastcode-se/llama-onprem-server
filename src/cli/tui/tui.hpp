#pragma once

#include "cli/tui/theme.hpp"
#include "cli/tui/ui.hpp"

#include <functional>

// Full-screen session. Calls body on a worker thread and returns its status.
int run_tui(const Theme &theme, const std::function<int(AgentUi &)> &body);
