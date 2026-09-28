#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <string>

// Collapsed: a frame around "▶ system". Expanded: the same frame around the full prompt.
[[nodiscard]] ftxui::Element system_prompt_block(const std::string &text, bool expanded, bool reveal,
                                                 ftxui::Color title_ink, ftxui::Color body_ink, bool bold,
                                                 ftxui::Box &hit);
