#include "cli/tui/tui_ui.hpp"

#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view kOwnAnswer = "Own answer";

std::vector<std::string> tool_menu_entries()
{
    return {"Yes, this once", "No", "Always this tool", "Full access"};
}

} // namespace

Ask TuiUi::ask(std::string title, std::string)
{
    {
        std::lock_guard lock(mutex);
        asking = true;
        question_mode = false;
        pick_mode = false;
        ask_title = std::move(title);
        menu = tool_menu_entries();
        menu_index = 0;
        ask_result = Ask::Deny;
        ask_done = false;
    }
    wake();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return ask_done || quit; });
    asking = false;
    return quit ? Ask::Closed : ask_result;
}

std::optional<std::string> TuiUi::question(std::string prompt, std::vector<std::string> choices)
{
    {
        std::lock_guard lock(mutex);
        asking = true;
        question_mode = true;
        pick_mode = false;
        ask_title = std::move(prompt);
        menu = std::move(choices);
        menu.push_back(std::string(kOwnAnswer));
        menu_index = 0;
        input.clear();
        cursor = 0;
        ask_text.clear();
        ask_done = false;
    }
    wake();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return ask_done || quit; });
    asking = false;
    if (quit || ask_text.empty())
    {
        return std::nullopt;
    }
    return ask_text;
}

std::optional<std::size_t> TuiUi::choose(std::string prompt, std::vector<std::string> choices, std::size_t selected)
{
    if (choices.empty())
    {
        return std::nullopt;
    }
    if (selected >= choices.size())
    {
        selected = 0;
    }
    {
        std::lock_guard lock(mutex);
        asking = true;
        question_mode = false;
        pick_mode = true;
        ask_title = std::move(prompt);
        menu = std::move(choices);
        menu_index = static_cast<int>(selected);
        ask_pick = -1;
        ask_done = false;
    }
    wake();
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return ask_done || quit; });
    asking = false;
    pick_mode = false;
    if (quit || ask_pick < 0 || static_cast<std::size_t>(ask_pick) >= menu.size())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(ask_pick);
}

