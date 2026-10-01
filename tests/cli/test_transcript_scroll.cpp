#include "cli/tui/transcript_scroll.hpp"
#include "../tests.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

namespace
{

ftxui::Element lines(int count)
{
    ftxui::Elements rows;
    for (int i = 0; i < count; ++i)
    {
        rows.push_back(ftxui::text(std::string(1, static_cast<char>('a' + i))));
    }
    return ftxui::vbox(std::move(rows));
}

std::string first_column(const ftxui::Screen &screen)
{
    std::string out;
    for (int y = 0; y < screen.dimy(); ++y)
    {
        if (y != 0)
        {
            out += '\n';
        }
        const std::string &cell = screen.PixelAt(0, y).character;
        out += cell.empty() ? " " : cell;
    }
    return out;
}

ftxui::Screen draw(ftxui::Element content, TranscriptScroll &scroll, int width, int height)
{
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, transcript_scroll(std::move(content), scroll));
    return screen;
}

int test_transcript_scroll_pins_to_bottom()
{
    TranscriptScroll scroll;
    const ftxui::Screen screen = draw(lines(8), scroll, 4, 4);
    assertEquals(4, scroll.max_offset);
    assertEquals(4, scroll.offset);
    assertTrue(scroll.follow_bottom);
    assertEquals(std::string("e\nf\ng\nh"), first_column(screen));
    return 0;
}

int test_transcript_scroll_wheel_up_then_growth_stays()
{
    TranscriptScroll scroll;
    draw(lines(8), scroll, 4, 4);
    assertTrue(transcript_scroll_wheel(scroll, true, 3));
    ftxui::Screen screen = draw(lines(8), scroll, 4, 4);
    assertEquals(1, scroll.offset);
    assertTrue(!scroll.follow_bottom);
    assertEquals(std::string("b\nc\nd\ne"), first_column(screen));

    screen = draw(lines(10), scroll, 4, 4);
    assertEquals(6, scroll.max_offset);
    assertEquals(1, scroll.offset);
    assertTrue(scroll.hold);
    assertTrue(!scroll.follow_bottom);
    screen = draw(lines(12), scroll, 4, 4);
    assertEquals(1, scroll.offset);
    assertEquals(std::string("b\nc\nd\ne"), first_column(screen));
    return 0;
}

int test_transcript_scroll_wheel_down_follows_tail_again()
{
    TranscriptScroll scroll;
    draw(lines(8), scroll, 4, 4);
    assertTrue(transcript_scroll_wheel(scroll, true, 3));
    draw(lines(10), scroll, 4, 4);
    assertTrue(transcript_scroll_wheel(scroll, false, 3));
    assertEquals(4, scroll.offset);
    assertTrue(!scroll.follow_bottom);
    assertTrue(transcript_scroll_wheel(scroll, false, 3));
    assertEquals(6, scroll.offset);
    assertTrue(scroll.follow_bottom);
    const ftxui::Screen screen = draw(lines(10), scroll, 4, 4);
    assertEquals(std::string("g\nh\ni\nj"), first_column(screen));
    assertTrue(!transcript_scroll_wheel(scroll, false, 3));
    return 0;
}

int test_transcript_scroll_fits_without_moving()
{
    TranscriptScroll scroll;
    const ftxui::Screen screen = draw(lines(2), scroll, 4, 4);
    assertEquals(0, scroll.max_offset);
    assertEquals(0, scroll.offset);
    assertEquals(std::string("a\nb\n \n "), first_column(screen));
    assertTrue(!transcript_scroll_wheel(scroll, true, 3));
    assertTrue(!transcript_scroll_wheel(scroll, false, 3));
    return 0;
}

int test_transcript_scroll_wheel_stops_at_top()
{
    TranscriptScroll scroll;
    scroll.max_offset = 6;
    scroll.offset = 0;
    scroll.follow_bottom = false;
    assertTrue(!transcript_scroll_wheel(scroll, true, 3));
    assertEquals(0, scroll.offset);
    return 0;
}

int test_transcript_scroll_follows_added_lines_from_bottom()
{
    TranscriptScroll scroll;
    ftxui::Screen screen = draw(lines(6), scroll, 4, 4);
    assertEquals(std::string("c\nd\ne\nf"), first_column(screen));
    assertTrue(scroll.follow_bottom);
    screen = draw(lines(9), scroll, 4, 4);
    assertTrue(scroll.follow_bottom);
    assertEquals(std::string("f\ng\nh\ni"), first_column(screen));
    return 0;
}

int test_transcript_scroll_follows_when_offset_is_at_max()
{
    TranscriptScroll scroll;
    draw(lines(8), scroll, 4, 4);
    assertTrue(transcript_scroll_wheel(scroll, true, 3));
    draw(lines(8), scroll, 4, 4);
    assertTrue(!scroll.follow_bottom);
    assertTrue(transcript_scroll_wheel(scroll, false, 3));
    assertEquals(scroll.max_offset, scroll.offset);
    assertTrue(scroll.follow_bottom);
    const ftxui::Screen screen = draw(lines(12), scroll, 4, 4);
    assertTrue(scroll.follow_bottom);
    assertEquals(std::string("i\nj\nk\nl"), first_column(screen));
    return 0;
}

int test_transcript_scroll_follows_wrapped_growth()
{
    const auto block = [](int words) {
        std::string text;
        for (int i = 0; i < words; ++i)
        {
            if (i != 0)
            {
                text += ' ';
            }
            text += "word" + std::to_string(i);
        }
        return ftxui::paragraph(text);
    };
    TranscriptScroll scroll;
    ftxui::Screen screen = draw(block(8), scroll, 10, 4);
    const std::string first = screen.ToString();
    assertTrue(scroll.follow_bottom);
    screen = draw(block(40), scroll, 10, 4);
    const std::string grown = screen.ToString();
    assertTrue(grown.find("word39") != std::string::npos);
    assertTrue(first.find("word39") == std::string::npos);
    return 0;
}

int test_transcript_scroll_visible_tail_follows_growth()
{
    TranscriptScroll scroll;
    draw(lines(8), scroll, 4, 4);
    scroll.follow_bottom = false;
    ftxui::Screen screen = draw(lines(8), scroll, 4, 4);
    assertTrue(scroll.follow_bottom);
    assertEquals(std::string("e\nf\ng\nh"), first_column(screen));
    screen = draw(lines(11), scroll, 4, 4);
    assertTrue(scroll.follow_bottom);
    assertEquals(std::string("h\ni\nj\nk"), first_column(screen));
    return 0;
}

int test_transcript_scroll_fitted_tail_follows_growth()
{
    TranscriptScroll scroll;
    draw(lines(2), scroll, 4, 4);
    scroll.follow_bottom = false;
    draw(lines(2), scroll, 4, 4);
    assertTrue(scroll.follow_bottom);
    const ftxui::Screen screen = draw(lines(8), scroll, 4, 4);
    assertEquals(std::string("e\nf\ng\nh"), first_column(screen));
    return 0;
}

int test_transcript_scroll_reveal_centers_focus()
{
    ftxui::Elements rows;
    for (int i = 0; i < 9; ++i)
    {
        ftxui::Element line = ftxui::text(std::string(1, static_cast<char>('a' + i)));
        if (i == 4)
        {
            line = std::move(line) | ftxui::focus;
        }
        rows.push_back(std::move(line));
    }
    TranscriptScroll scroll;
    scroll.follow_bottom = false;
    scroll.follow_reveal = true;
    const ftxui::Screen screen = draw(ftxui::vbox(std::move(rows)), scroll, 4, 3);
    assertEquals(3, scroll.offset);
    assertEquals(std::string("d\ne\nf"), first_column(screen));
    return 0;
}

} // namespace

int test_transcript_scroll()
{
    RUN_TEST(test_transcript_scroll_pins_to_bottom);
    RUN_TEST(test_transcript_scroll_follows_added_lines_from_bottom);
    RUN_TEST(test_transcript_scroll_follows_when_offset_is_at_max);
    RUN_TEST(test_transcript_scroll_wheel_up_then_growth_stays);
    RUN_TEST(test_transcript_scroll_wheel_down_follows_tail_again);
    RUN_TEST(test_transcript_scroll_fits_without_moving);
    RUN_TEST(test_transcript_scroll_wheel_stops_at_top);
    RUN_TEST(test_transcript_scroll_follows_wrapped_growth);
    RUN_TEST(test_transcript_scroll_visible_tail_follows_growth);
    RUN_TEST(test_transcript_scroll_fitted_tail_follows_growth);
    RUN_TEST(test_transcript_scroll_reveal_centers_focus);
    return 0;
}
