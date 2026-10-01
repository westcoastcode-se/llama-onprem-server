#include "cli/tui/tui_ui.hpp"
#include "cli/tui/tui.hpp"
#include "cli/tui/system_block.hpp"
#include "cli/tui/thinking_block.hpp"
#include "cli/tui/transcript_scroll.hpp"

#include <ftxui/screen/color.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <condition_variable>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>


namespace
{
struct ThinkPreview
{
    std::string line;
    bool more = false;
};

ThinkPreview think_preview(std::string_view text)
{
    std::size_t start = 0;
    while (start < text.size() &&
           (text[start] == '\n' || text[start] == '\r' || text[start] == ' ' || text[start] == '\t'))
    {
        ++start;
    }
    std::size_t end = start;
    while (end < text.size() && text[end] != '\n' && text[end] != '\r')
    {
        ++end;
    }
    std::string line(text.substr(start, end - start));
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
    {
        line.pop_back();
    }
    std::size_t rest = end;
    while (rest < text.size() &&
           (text[rest] == '\n' || text[rest] == '\r' || text[rest] == ' ' || text[rest] == '\t'))
    {
        ++rest;
    }
    return {std::move(line), rest < text.size()};
}

std::string fit_columns(const std::string &line, int columns)
{
    if (columns <= 0)
    {
        return {};
    }
    if (ftxui::string_width(line) <= columns)
    {
        return line;
    }
    if (columns == 1)
    {
        return "…";
    }
    std::string out;
    int used = 0;
    const int limit = columns - 1;
    for (const std::string &glyph : ftxui::Utf8ToGlyphs(line))
    {
        if (glyph.empty())
        {
            continue;
        }
        const int width = std::max(1, ftxui::string_width(glyph));
        if (used + width > limit)
        {
            break;
        }
        out += glyph;
        used += width;
    }
    out += "…";
    return out;
}

std::string fit_label(std::string label, int columns, bool more)
{
    const bool too_wide = ftxui::string_width(label) > columns;
    if (!too_wide && !more)
    {
        return label;
    }
    if (!too_wide && ftxui::string_width(label) + 1 <= columns)
    {
        label += "…";
        return label;
    }
    return fit_columns(label, columns);
}

std::string without_cr(std::string text)
{
    std::erase(text, '\r');
    return text;
}

ftxui::Color paint(const ThemeColor &color)
{
    if (color.palette >= 0 && color.palette <= 15)
    {
        return ftxui::Color(static_cast<ftxui::Color::Palette16>(color.palette));
    }
    return ftxui::Color::RGBA(color.r, color.g, color.b, 255);
}

ftxui::Element weighted(ftxui::Element element, const Theme &theme)
{
    if (!theme.bold)
    {
        return element;
    }
    return std::move(element) | ftxui::bold;
}

int text_columns()
{
    return std::max(16, ftxui::Terminal::Size().dimx - 2);
}

bool foldable(const Block &block)
{
    return block.kind == "thinking" || block.kind == "tool" || block.kind == "system";
}

ftxui::Element tool_block(const Block &block, bool reveal, ftxui::Box &hit, const Theme &theme)
{
    using namespace ftxui;
    const Color ink = paint(theme.tool);
    const int columns = text_columns();
    const ThinkPreview preview = think_preview(block.text);
    std::string title = block.caption.empty() ? preview.line : block.caption;
    if (title.empty())
    {
        title = "tool";
    }
    const bool more = !preview.line.empty() || preview.more;
    Elements lines;
    if (!block.expanded)
    {
        lines.push_back(text("▶ " + fit_label(title, columns, more)) | color(ink));
    }
    else
    {
        Element header = weighted(text("▼ " + title), theme) | color(ink);
        if (reveal)
        {
            header = header | focus;
        }
        lines.push_back(std::move(header));
        const std::string clean = without_cr(block.text);
        if (!clean.empty())
        {
            lines.push_back(paragraph(clean) | color(paint(theme.tool_text)));
        }
    }
    return vbox(std::move(lines)) | reflect(hit);
}

std::string trimmed_edges(const std::string &text)
{
    std::string clean = without_cr(text);
    std::size_t begin = 0;
    while (begin < clean.size() && clean[begin] == '\n')
    {
        ++begin;
    }
    std::size_t end = clean.size();
    while (end > begin && clean[end - 1] == '\n')
    {
        --end;
    }
    return clean.substr(begin, end - begin);
}

ftxui::Element assistant_block(const Block &block, const Theme &theme)
{
    using namespace ftxui;
    const std::string clean = trimmed_edges(block.text);
    const std::string title = block.caption.empty() ? "assistant" : block.caption;
    const Color ink = paint(theme.text);
    Elements lines;
    lines.push_back(text(title) | dim | color(ink));
    if (!clean.empty())
    {
        lines.push_back(paragraph(clean) | color(ink));
    }
    return vbox(std::move(lines));
}

std::size_t utf8_prev(std::string_view text, std::size_t index)
{
    if (index == 0 || index > text.size())
    {
        return 0;
    }
    --index;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80)
    {
        --index;
    }
    return index;
}

std::size_t utf8_next(std::string_view text, std::size_t index)
{
    if (index >= text.size())
    {
        return text.size();
    }
    ++index;
    while (index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80)
    {
        ++index;
    }
    return index;
}

constexpr int kWheelLines = 3;



std::string format_token_count(int count)
{
    if (count < 1000)
    {
        return std::to_string(std::max(0, count));
    }
    const double thousands = count / 1000.0;
    if (count < 10000)
    {
        return std::format("{:.1f}k", thousands);
    }
    return std::format("{:.0f}k", thousands);
}

Ask tool_ask_from_index(int index)
{
    switch (index)
    {
    case 0:
        return Ask::Once;
    case 2:
        return Ask::AlwaysTool;
    case 3:
        return Ask::Full;
    default:
        return Ask::Deny;
    }
}

} // namespace

void TuiUi::toggle_fold(std::size_t index)
{
    if (index >= blocks.size() || !foldable(blocks[index]))
    {
        return;
    }
    blocks[index].expanded = !blocks[index].expanded;
    if (blocks[index].expanded)
    {
        revealed = index;
        scroll.follow_reveal = true;
        scroll.follow_bottom = false;
    }
    else if (revealed == index)
    {
        revealed = static_cast<std::size_t>(-1);
        scroll.follow_reveal = false;
        scroll.follow_bottom = true;
    }
}

ftxui::Element TuiUi::transcript()
{
    using namespace ftxui;
    std::lock_guard lock(mutex);
    Elements rows;
    think_boxes.assign(blocks.size(), Box{0, -1, 0, -1});
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const Block &block = blocks[i];
        if (foldable(block))
        {
            const bool reveal = block.expanded && revealed == i;
            if (block.kind == "thinking")
            {
                const std::string title = block.caption.empty() ? "thinking" : block.caption;
                rows.push_back(thinking_transcript_block(title, block.text, block.expanded, block.thinking_live,
                                                         block.think_ms, reveal, theme.bold, paint(theme.thinking_text),
                                                         think_boxes[i]));
            }
            else if (block.kind == "system")
            {
                rows.push_back(system_prompt_block(block.text, block.expanded, reveal, paint(theme.note),
                                                   paint(theme.text), theme.bold, think_boxes[i]));
            }
            else
            {
                rows.push_back(tool_block(block, reveal, think_boxes[i], theme));
            }
            rows.push_back(separatorEmpty());
            continue;
        }
        if (block.kind == "assistant")
        {
            if (!trimmed_edges(block.text).empty())
            {
                rows.push_back(assistant_block(block, theme));
                rows.push_back(separatorEmpty());
            }
            continue;
        }
        Color ink = paint(theme.text);
        if (block.kind == "you")
        {
            ink = paint(theme.you);
        }
        else if (block.kind == "note")
        {
            ink = paint(theme.note);
        }
        rows.push_back(text(block.kind) | dim | color(ink));
        std::istringstream lines(block.text);
        std::string line_text;
        bool any = false;
        while (std::getline(lines, line_text))
        {
            any = true;
            rows.push_back(text(line_text) | color(ink));
        }
        if (!any && !block.text.empty())
        {
            rows.push_back(text(block.text) | color(ink));
        }
        rows.push_back(separatorEmpty());
    }
    if (rows.empty())
    {
        rows.push_back(text("Ask for a change, or /help") | dim);
    }
    Element body = vbox(std::move(rows));
    return transcript_scroll(std::move(body), scroll);
}

bool TuiUi::on_event(ftxui::Event event)
{
    using namespace ftxui;
    if (event == Event::CtrlC || event == Event::CtrlD)
    {
        g_agent_interrupt.store(true, std::memory_order_relaxed);
        std::lock_guard lock(mutex);
        if (asking)
        {
            ask_done = true;
            ask_result = Ask::Closed;
            ask_pick = -1;
            asking = false;
        }
        if (event == Event::CtrlD)
        {
            quit = true;
            line_ready = true;
        }
        cv.notify_all();
        return true;
    }
    if (event.is_mouse())
    {
        const Mouse &mouse = event.mouse();
        if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown)
        {
            if (!scroll.viewport.Contain(mouse.x, mouse.y))
            {
                return false;
            }
            return transcript_scroll_wheel(scroll, mouse.button == Mouse::WheelUp, kWheelLines);
        }
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed)
        {
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < think_boxes.size() && i < blocks.size(); ++i)
            {
                if (think_boxes[i].Contain(mouse.x, mouse.y))
                {
                    toggle_fold(i);
                    return true;
                }
            }
        }
        return false;
    }
    if (event == Event::CtrlO)
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = blocks.size(); i > 0; --i)
        {
            if (foldable(blocks[i - 1]))
            {
                toggle_fold(i - 1);
                return true;
            }
        }
        return true;
    }
    if (event == Event::ArrowUp || event == Event::ArrowDown)
    {
        std::lock_guard lock(mutex);
        if (!asking || menu.empty())
        {
            return asking;
        }
        const int count = static_cast<int>(menu.size());
        if (menu_index < 0 || menu_index >= count)
        {
            menu_index = 0;
        }
        else if (event == Event::ArrowUp)
        {
            menu_index = (menu_index + count - 1) % count;
        }
        else
        {
            menu_index = (menu_index + 1) % count;
        }
        return true;
    }
    if (asking && !question_mode && event.is_character())
    {
        const std::string key = event.character();
        Ask choice = Ask::Deny;
        if (key == "y" || key == "Y")
        {
            choice = Ask::Once;
        }
        else if (key == "a" || key == "A")
        {
            choice = Ask::AlwaysTool;
        }
        else if (key == "f" || key == "F")
        {
            choice = Ask::Full;
        }
        else if (key != "n" && key != "N")
        {
            return true;
        }
        std::lock_guard lock(mutex);
        ask_result = choice;
        ask_done = true;
        asking = false;
        cv.notify_all();
        return true;
    }
    if (event == Event::Escape)
    {
        std::lock_guard lock(mutex);
        if (asking && pick_mode)
        {
            ask_pick = -1;
            ask_done = true;
            asking = false;
            cv.notify_all();
            return true;
        }
        return false;
    }
    if (event == Event::Return)
    {
        std::lock_guard lock(mutex);
        if (asking && pick_mode)
        {
            if (menu.empty() || menu_index < 0 || menu_index >= static_cast<int>(menu.size()))
            {
                return true;
            }
            ask_pick = menu_index;
            ask_done = true;
            asking = false;
            cv.notify_all();
            return true;
        }
        if (asking && question_mode)
        {
            if (menu.empty() || menu_index < 0 || menu_index >= static_cast<int>(menu.size()))
            {
                return true;
            }
            const bool own = menu_index == static_cast<int>(menu.size()) - 1;
            if (own)
            {
                if (input.empty())
                {
                    return true;
                }
                ask_text = input;
            }
            else
            {
                ask_text = menu[static_cast<std::size_t>(menu_index)];
            }
            input.clear();
            cursor = 0;
            ask_done = true;
            asking = false;
            cv.notify_all();
            return true;
        }
        if (asking)
        {
            ask_result = tool_ask_from_index(menu_index);
            ask_done = true;
            asking = false;
            cv.notify_all();
            return true;
        }
        if (input.empty())
        {
            return true;
        }
        line = input;
        input.clear();
        cursor = 0;
        line_ready = true;
        scroll.follow_bottom = true;
        scroll.follow_reveal = false;
        cv.notify_all();
        return true;
    }
    if (event == Event::ArrowLeft || event == Event::ArrowRight || event == Event::Home || event == Event::End ||
        event == Event::Backspace || event == Event::Delete)
    {
        std::lock_guard lock(mutex);
        if (asking && !question_mode)
        {
            return true;
        }
        cursor = std::min(cursor, input.size());
        if (event == Event::ArrowLeft || event == Event::Backspace)
        {
            const std::size_t previous = utf8_prev(input, cursor);
            if (event == Event::Backspace && previous < cursor)
            {
                input.erase(previous, cursor - previous);
            }
            cursor = previous;
        }
        else if (event == Event::ArrowRight || event == Event::Delete)
        {
            const std::size_t next = utf8_next(input, cursor);
            if (event == Event::Delete && next > cursor)
            {
                input.erase(cursor, next - cursor);
            }
            else
            {
                cursor = next;
            }
        }
        else if (event == Event::Home)
        {
            cursor = 0;
        }
        else
        {
            cursor = input.size();
        }
        return true;
    }
    if (event.is_character())
    {
        std::lock_guard lock(mutex);
        if (asking && !question_mode)
        {
            return true;
        }
        if (asking && question_mode && !menu.empty())
        {
            menu_index = static_cast<int>(menu.size()) - 1;
        }
        cursor = std::min(cursor, input.size());
        input.insert(cursor, event.character());
        cursor += event.character().size();
        return true;
    }
    return false;
}

ftxui::Element TuiUi::render()
{
    using namespace ftxui;
    std::string status_copy;
    int used_copy = 0;
    int size_copy = 0;
    std::string input_copy;
    std::size_t cursor_copy = 0;
    std::string title;
    std::vector<std::string> menu_copy;
    int menu_index_copy = 0;
    bool show_ask = false;
    bool show_question = false;
    bool show_pick = false;
    {
        std::lock_guard lock(mutex);
        status_copy = status;
        used_copy = context_used;
        size_copy = context_size;
        input_copy = input;
        cursor_copy = std::min(cursor, input.size());
        show_ask = asking;
        show_question = question_mode;
        show_pick = pick_mode;
        title = ask_title;
        menu_copy = menu;
        menu_index_copy = menu_index;
        ready = true;
        cv.notify_all();
    }
    const std::string before = input_copy.substr(0, cursor_copy);
    const std::string after = input_copy.substr(cursor_copy);
    Element caret;
    if (after.empty())
    {
        caret = text(" ") | inverted;
    }
    else
    {
        const std::size_t next = utf8_next(after, 0);
        caret = hbox({text(after.substr(0, next)) | inverted, text(after.substr(next))});
    }
    Element composer = hbox({weighted(text("> "), theme), text(before), caret});
    if (show_ask && !show_question)
    {
        composer = composer | dim;
    }
    Elements column;
    column.push_back(transcript());
    if (show_ask)
    {
        Elements panel;
        panel.push_back(weighted(text(title), theme) | color(paint(theme.prompt)));
        if (menu_index_copy < 0 || menu_index_copy >= static_cast<int>(menu_copy.size()))
        {
            menu_index_copy = 0;
        }
        for (int i = 0; i < static_cast<int>(menu_copy.size()); ++i)
        {
            const bool selected = i == menu_index_copy;
            Element label = paragraph(menu_copy[static_cast<std::size_t>(i)]) | flex;
            Element row = hbox({text(selected ? "> " : "  "), label});
            if (selected)
            {
                row = row | inverted;
            }
            panel.push_back(row);
        }
        if (show_pick)
        {
            panel.push_back(text("Up and down select. Enter chooses. Esc cancels.") | dim);
        }
        else if (show_question)
        {
            panel.push_back(text("Up and down select. Enter sends the row. Type your own answer, then Enter.") | dim);
        }
        else
        {
            panel.push_back(text("Up and down select. Enter confirms.  y once   n no   a always   f full") | dim);
        }
        column.push_back(separator());
        Element ask = vbox(std::move(panel));
        if (theme.border)
        {
            ask = ask | borderStyled(theme.rounded ? ROUNDED : LIGHT);
        }
        column.push_back(ask);
    }
    column.push_back(separator());
    column.push_back(composer);
    Element main = vbox(std::move(column)) | flex;
    Element meter = text("");
    if (size_copy > 0)
    {
        const int used = std::max(0, used_copy);
        int percent = static_cast<int>((100LL * used + size_copy / 2) / size_copy);
        if (percent > 999)
        {
            percent = 999;
        }
        const std::string label = std::format("{}%  {}/{}", percent, format_token_count(used), format_token_count(size_copy));
        Color ink = paint(theme.meter);
        if (percent >= 90)
        {
            ink = paint(theme.meter_hot);
        }
        else if (percent >= 75)
        {
            ink = paint(theme.meter_warn);
        }
        meter = text(label) | color(ink);
    }
    Element header = dbox({
        text(status_copy) | dim,
        hbox({filler(), meter}),
    });
    Element screen = vbox({
        header,
        separator(),
        main,
    });
    if (!theme.border)
    {
        return screen | flex;
    }
    return window(text(" callisto "), std::move(screen), theme.rounded ? ROUNDED : LIGHT) | flex;
}

int run_tui(const Theme &theme, const std::function<int(AgentUi &)> &body)
{
    using namespace ftxui;
    auto screen = ScreenInteractive::Fullscreen();
    auto ui = std::make_shared<TuiUi>(theme);
    ui->screen = &screen;

    int status = 0;
    std::thread worker([&] {
        {
            std::unique_lock lock(ui->mutex);
            ui->cv.wait(lock, [&] { return ui->ready; });
        }
        try
        {
            status = body(*ui);
        }
        catch (const std::exception &error)
        {
            ui->note(error.what());
            status = 1;
        }
        screen.Exit();
    });

    auto component = Renderer([ui] { return ui->render(); });
    component |= CatchEvent([ui](Event event) { return ui->on_event(event); });
    screen.ForceHandleCtrlC(false);
    screen.ForceHandleCtrlZ(false);
    screen.Loop(component);
    worker.join();
    return status;
}
