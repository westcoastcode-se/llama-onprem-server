#include "cli/tui/tui_ui.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

std::size_t TuiUi::latest(std::string_view kind) const
{
    for (std::size_t index = blocks.size(); index > 0; --index)
    {
        if (blocks[index - 1].kind == kind)
        {
            return index - 1;
        }
    }
    return static_cast<std::size_t>(-1);
}

void TuiUi::wake()
{
    if (screen != nullptr)
    {
        // An empty task does not invalidate the frame, so the screen stays
        // frozen until the next key. Custom does.
        screen->PostEvent(ftxui::Event::Custom);
    }
}

void TuiUi::set_status(std::string text)
{
    {
        std::lock_guard lock(mutex);
        status = std::move(text);
    }
    wake();
}

void TuiUi::set_context(int used, int size)
{
    {
        std::lock_guard lock(mutex);
        context_used = std::max(0, used);
        context_size = std::max(0, size);
    }
    wake();
}

void TuiUi::note(std::string text)
{
    {
        std::lock_guard lock(mutex);
        seal_open_block();
        blocks.push_back(Block{"note", std::move(text)});
    }
    wake();
}

void TuiUi::set_subagent_live(bool on)
{
    std::lock_guard lock(mutex);
    subagent_live = on;
    wake();
}

void TuiUi::begin(std::string kind)
{
    {
        std::lock_guard lock(mutex);
        seal_open_block();
        const bool thinking = kind == "thinking";
        blocks.push_back(Block{std::move(kind), {}});
        if (subagent_live)
        {
            blocks.back().caption = "sub-agent";
        }
        if (thinking)
        {
            blocks.back().expanded = true;
            blocks.back().thinking_live = true;
            blocks.back().think_started = std::chrono::steady_clock::now();
        }
        open = true;
    }
    wake();
}

void TuiUi::append(std::string text)
{
    {
        std::lock_guard lock(mutex);
        if (!open)
        {
            blocks.push_back(Block{"assistant", {}});
            open = true;
            if (subagent_live)
            {
                blocks.back().caption = "sub-agent";
            }
        }
        blocks.back().text += text;
    }
    wake();
}

void TuiUi::end()
{
    {
        std::lock_guard lock(mutex);
        seal_open_block();
    }
    wake();
}

void TuiUi::seal_open_block()
{
    if (!open || blocks.empty())
    {
        open = false;
        return;
    }
    open = false;
    Block &block = blocks.back();
    if (block.kind != "thinking" || !block.thinking_live)
    {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               block.think_started);
    block.think_ms = std::max<std::int64_t>(elapsed.count(), 0);
    block.thinking_live = false;
    block.expanded = false;
    const std::size_t index = blocks.size() - 1;
    if (revealed == index)
    {
        revealed = static_cast<std::size_t>(-1);
        scroll.follow_reveal = false;
        scroll.follow_bottom = true;
    }
}

void TuiUi::expand(std::string_view kind)
{
    {
        std::lock_guard lock(mutex);
        const std::size_t index = latest(kind);
        if (index != static_cast<std::size_t>(-1))
        {
            blocks[index].expanded = true;
        }
    }
    wake();
}

void TuiUi::collapse(std::string_view kind)
{
    {
        std::lock_guard lock(mutex);
        const std::size_t index = latest(kind);
        if (index == static_cast<std::size_t>(-1))
        {
            return;
        }
        blocks[index].expanded = false;
        if (revealed == index)
        {
            revealed = static_cast<std::size_t>(-1);
            scroll.follow_reveal = false;
            scroll.follow_bottom = true;
        }
    }
    wake();
}

void TuiUi::caption(std::string text)
{
    {
        std::lock_guard lock(mutex);
        if (open && !blocks.empty())
        {
            blocks.back().caption = std::move(text);
        }
    }
    wake();
}

void TuiUi::clear()
{
    {
        std::lock_guard lock(mutex);
        blocks.clear();
        open = false;
        revealed = static_cast<std::size_t>(-1);
        scroll = {};
    }
    wake();
}

void TuiUi::show_saved_thinking(std::string text)
{
    if (text.empty())
    {
        return;
    }
    {
        std::lock_guard lock(mutex);
        seal_open_block();
        Block block{"thinking", std::move(text)};
        block.thinking_live = false;
        block.think_ms = -1;
        blocks.push_back(std::move(block));
    }
    wake();
}

void TuiUi::show_system(std::string text)
{
    if (text.empty())
    {
        return;
    }
    {
        std::lock_guard lock(mutex);
        seal_open_block();
        blocks.push_back(Block{"system", std::move(text)});
    }
    wake();
}

