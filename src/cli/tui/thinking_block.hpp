#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// The last max_lines lines. A single trailing newline is not its own line.
[[nodiscard]] std::string thinking_tail(std::string_view text, int max_lines);

// Milliseconds under one second, otherwise tenths of a second (1.2s).
[[nodiscard]] std::string format_think_duration(std::int64_t milliseconds);

// Live and open: header plus the last three lines.
// Finished and closed: header plus how long thinking took.
// Finished and open: that header plus the full text.
[[nodiscard]] ftxui::Element thinking_transcript_block(std::string_view title, std::string_view content, bool expanded,
                                                       bool live, std::int64_t duration_ms, bool reveal, bool bold,
                                                       ftxui::Color ink, ftxui::Box &hit);
