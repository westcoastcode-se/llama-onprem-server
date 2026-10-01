#include "cli/tui/thinking_block.hpp"

#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace
{

constexpr int kTailLines = 3;

std::string without_cr(std::string text)
{
    std::erase(text, '\r');
    return text;
}

int text_columns()
{
    return std::max(16, ftxui::Terminal::Size().dimx - 2);
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

std::string header_label(std::string_view title, bool live, std::int64_t duration_ms)
{
    std::string label(title.empty() ? "thinking" : title);
    if (!live && duration_ms >= 0)
    {
        label += "  ";
        label += format_think_duration(duration_ms);
    }
    return label;
}

} // namespace

std::string thinking_tail(std::string_view text, int max_lines)
{
    if (max_lines <= 0 || text.empty())
    {
        return {};
    }
    std::string clean = without_cr(std::string(text));
    if (!clean.empty() && clean.back() == '\n')
    {
        clean.pop_back();
    }
    if (clean.empty())
    {
        return {};
    }
    int breaks = 0;
    std::size_t start = 0;
    for (std::size_t index = clean.size(); index > 0; --index)
    {
        if (clean[index - 1] != '\n')
        {
            continue;
        }
        ++breaks;
        if (breaks == max_lines)
        {
            start = index;
            break;
        }
    }
    return clean.substr(start);
}

std::string format_think_duration(std::int64_t milliseconds)
{
    const auto ms = std::max<std::int64_t>(milliseconds, 0);
    if (ms < 1000)
    {
        return std::format("{}ms", ms);
    }
    const auto tenths = (ms + 50) / 100;
    return std::format("{}.{}s", tenths / 10, tenths % 10);
}

ftxui::Element thinking_transcript_block(std::string_view title, std::string_view content, bool expanded, bool live,
                                         std::int64_t duration_ms, bool reveal, bool bold, ftxui::Color ink,
                                         ftxui::Box &hit)
{
    using namespace ftxui;
    const std::string label = header_label(title, live, duration_ms);
    std::string header = std::string(expanded ? "▼ " : "▶ ") + label;
    if (!expanded)
    {
        header = fit_columns(header, text_columns());
    }
    Element head = text(header) | color(ink);
    if (expanded && bold)
    {
        head = std::move(head) | ftxui::bold;
    }
    if (reveal)
    {
        head = std::move(head) | focus;
    }
    Elements lines;
    lines.push_back(std::move(head));
    if (expanded)
    {
        const std::string body = live ? thinking_tail(content, kTailLines) : without_cr(std::string(content));
        if (!body.empty())
        {
            lines.push_back(paragraph(body) | color(ink));
        }
    }
    return vbox(std::move(lines)) | reflect(hit);
}
