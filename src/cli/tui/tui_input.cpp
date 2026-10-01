#include "cli/tui/tui_ui.hpp"

#include <utility>

std::string TuiUi::read_line()
{
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return line_ready || quit; });
    if (quit)
    {
        return {};
    }
    line_ready = false;
    return std::move(line);
}

