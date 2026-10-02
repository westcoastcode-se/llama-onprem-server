#include "cli/tui/system_block.hpp"
#include "../tests.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

namespace
{

std::string draw(ftxui::Element element, int width, int height)
{
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, std::move(element));
    return screen.ToString();
}

// The block sits in a column, the way the transcript lays it out, so a tall
// screen must not stretch the collapsed row.
std::string draw_in_column(ftxui::Element element, int width, int height)
{
    return draw(ftxui::vbox({std::move(element), ftxui::text("AFTER")}), width, height);
}

ftxui::Element block(const std::string &text, bool expanded, bool bold, ftxui::Box &hit)
{
    return system_prompt_block(text, expanded, false, ftxui::Color::White, ftxui::Color::White, bold, hit);
}

std::string line_at(const std::string &drawn, int index)
{
    std::size_t start = 0;
    for (int row = 0; row < index; ++row)
    {
        const std::size_t end = drawn.find('\n', start);
        if (end == std::string::npos)
        {
            return {};
        }
        start = end + 1;
    }
    const std::size_t end = drawn.find('\n', start);
    std::string line = drawn.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    return line;
}

int test_system_block_hides_prompt_until_opened()
{
    const std::string prompt = "ALPHA first line of the prompt\nBETA stays inside until opened";
    ftxui::Box hit;
    const std::string closed = draw_in_column(block(prompt, false, true, hit), 42, 8);
    assertTrue(line_at(closed, 0).find("▶ system") != std::string::npos);
    assertTrue(line_at(closed, 1).starts_with("AFTER"));
    assertTrue(closed.find("ALPHA") == std::string::npos);
    assertTrue(closed.find("BETA") == std::string::npos);
    assertTrue(hit.y_max == hit.y_min);

    ftxui::Box open_hit;
    const std::string opened = draw_in_column(block(prompt, true, true, open_hit), 42, 8);
    assertTrue(opened.find("ALPHA") != std::string::npos);
    assertTrue(opened.find("BETA") != std::string::npos);
    assertTrue(opened.find("▼") != std::string::npos);
    return 0;
}

} // namespace

int test_system_block()
{
    RUN_TEST(test_system_block_hides_prompt_until_opened);
    return 0;
}
