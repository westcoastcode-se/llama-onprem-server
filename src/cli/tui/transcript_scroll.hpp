#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

// Vertical viewport for the session transcript.
// follow_bottom keeps the latest line on screen as text arrives.
// hold means the user scrolled up; new text must not pull the view back down
// until the wheel returns to the bottom.
// follow_reveal keeps a focused row (an opened thinking or tool block) on screen.
// The wheel moves offset by whole lines.
struct TranscriptScroll
{
    int offset = 0;
    int max_offset = 0;
    bool follow_bottom = true;
    bool follow_reveal = false;
    bool hold = false;
    ftxui::Box viewport{};
};

ftxui::Element transcript_scroll(ftxui::Element child, TranscriptScroll &scroll);

// up moves toward older lines. True when the viewport will change.
bool transcript_scroll_wheel(TranscriptScroll &scroll, bool up, int step);
