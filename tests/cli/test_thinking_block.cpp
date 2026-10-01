#include "cli/tui/thinking_block.hpp"
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

std::string draw_in_column(ftxui::Element element, int width, int height)
{
    return draw(ftxui::vbox({std::move(element), ftxui::text("AFTER")}), width, height);
}

ftxui::Element block(const std::string &text, bool expanded, bool live, std::int64_t duration_ms, ftxui::Box &hit)
{
    return thinking_transcript_block("thinking", text, expanded, live, duration_ms, false, true, ftxui::Color::White,
                                     hit);
}

int test_thinking_tail_keeps_the_last_three_lines()
{
    assertEquals(std::string("BETA\nGAMMA\nDELTA"), thinking_tail("ALPHA\nBETA\nGAMMA\nDELTA\n", 3));
    assertEquals(std::string("BETA\nGAMMA\nDEL"), thinking_tail("ALPHA\nBETA\nGAMMA\nDEL", 3));
    assertEquals(std::string("BETA\nGAMMA\nDELTA"), thinking_tail("ALPHA\r\nBETA\r\nGAMMA\r\nDELTA\r\n", 3));
    assertEquals(std::string("ALPHA\nBETA"), thinking_tail("ALPHA\nBETA", 3));
    assertEquals(std::string(""), thinking_tail("", 3));
    assertEquals(std::string(""), thinking_tail("\n", 3));
    assertEquals(std::string(""), thinking_tail("ALPHA\nBETA", 0));
    return 0;
}

int test_format_think_duration_uses_tenths_past_one_second()
{
    assertEquals(std::string("0ms"), format_think_duration(0));
    assertEquals(std::string("0ms"), format_think_duration(-4));
    assertEquals(std::string("999ms"), format_think_duration(999));
    assertEquals(std::string("1.0s"), format_think_duration(1000));
    assertEquals(std::string("1.0s"), format_think_duration(1040));
    assertEquals(std::string("1.1s"), format_think_duration(1050));
    assertEquals(std::string("1.5s"), format_think_duration(1500));
    return 0;
}

int test_live_thinking_shows_only_the_last_three_lines()
{
    const std::string text = "ALPHA stays hidden\nBETA second\nGAMMA third\nDELTA newest";
    ftxui::Box hit;
    const std::string drawn = draw_in_column(block(text, true, true, -1, hit), 42, 8);
    assertTrue(drawn.find("▼ thinking") != std::string::npos);
    assertTrue(drawn.find("BETA second") != std::string::npos);
    assertTrue(drawn.find("GAMMA third") != std::string::npos);
    assertTrue(drawn.find("DELTA newest") != std::string::npos);
    assertTrue(drawn.find("ALPHA") == std::string::npos);
    assertTrue(drawn.find("ms") == std::string::npos);
    assertTrue(hit.y_max > hit.y_min);
    return 0;
}

int test_finished_thinking_collapses_to_the_header_and_duration()
{
    const std::string text = "ALPHA stays hidden\nBETA second\nGAMMA third\nDELTA newest";
    ftxui::Box hit;
    const std::string closed = draw_in_column(block(text, false, false, 1200, hit), 42, 8);
    assertTrue(closed.find("▶ thinking  1.2s") != std::string::npos);
    assertTrue(closed.find("ALPHA") == std::string::npos);
    assertTrue(closed.find("DELTA") == std::string::npos);
    assertTrue(hit.y_max == hit.y_min);

    ftxui::Box open_hit;
    const std::string opened = draw_in_column(block(text, true, false, 1200, open_hit), 42, 8);
    assertTrue(opened.find("▼ thinking  1.2s") != std::string::npos);
    assertTrue(opened.find("ALPHA stays hidden") != std::string::npos);
    assertTrue(opened.find("DELTA newest") != std::string::npos);
    return 0;
}

} // namespace

int test_thinking_block()
{
    RUN_TEST(test_thinking_tail_keeps_the_last_three_lines);
    RUN_TEST(test_format_think_duration_uses_tenths_past_one_second);
    RUN_TEST(test_live_thinking_shows_only_the_last_three_lines);
    RUN_TEST(test_finished_thinking_collapses_to_the_header_and_duration);
    return 0;
}
