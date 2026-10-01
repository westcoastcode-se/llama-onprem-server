#include "cli/tui/system_block.hpp"

#include <string>
#include <utility>

namespace
{

std::string without_cr(std::string text)
{
    std::erase(text, '\r');
    return text;
}

ftxui::Element title_line(std::string label, ftxui::Color ink, bool bold, bool reveal)
{
    using namespace ftxui;
    Element title = text(std::move(label)) | color(ink);
    if (bold)
    {
        title = std::move(title) | ftxui::bold;
    }
    if (reveal)
    {
        title = std::move(title) | focus;
    }
    return title;
}

} // namespace

ftxui::Element system_prompt_block(const std::string &text, bool expanded, bool reveal, ftxui::Color title_ink,
                                   ftxui::Color body_ink, bool bold, ftxui::Box &hit)
{
    using namespace ftxui;
    const std::string clean = without_cr(text);
    Elements lines;
    // The header carries focus so opening the box keeps that row on screen.
    // A window title does not, because the frame copies focus from its body.
    lines.push_back(title_line(expanded ? "▼ system" : "▶ system", title_ink, bold, reveal));
    if (expanded && !clean.empty())
    {
        lines.push_back(paragraph(clean) | color(body_ink));
    }
    return vbox(std::move(lines)) | reflect(hit);
}
