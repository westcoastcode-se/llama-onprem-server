#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// One terminal color. palette is a 16-color index (0-15) when it is not negative.
// Otherwise the color is the RGB triple.
struct ThemeColor
{
    int palette = -1;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    [[nodiscard]] std::string ansi() const;
};

// Colors the client paints. Missing file and an empty spec use "default",
// which is the palette the interface had before themes.
struct Theme
{
    std::string name;
    // Border around an open thinking block.
    ThemeColor thinking;
    // Thinking text, both while the block is one line and after it is opened.
    ThemeColor thinking_text;
    ThemeColor surface;
    ThemeColor tool;
    ThemeColor tool_text;
    ThemeColor you;
    ThemeColor note;
    ThemeColor text;
    ThemeColor prompt;
    ThemeColor meter;
    ThemeColor meter_warn;
    ThemeColor meter_hot;
    // Frame around the whole window and the question panel. Transcript blocks stay flush left.
    bool border = true;
    // Rounded corners when border is set. Otherwise the frame is square.
    bool rounded = true;
    // Bold titles on thinking and tool blocks, the prompt marker, and questions.
    bool bold = true;
};

// default, ink, nord, forest, or ember. Anything else throws.
[[nodiscard]] Theme theme_builtin(std::string_view name);

// Object with an optional "theme" base name and color overrides.
// A color is a palette name (yellow, cyan, gray, graydark, white, ...) or #rgb / #rrggbb.
[[nodiscard]] Theme theme_from_json(std::string_view text);

// $XDG_CONFIG_HOME/callisto/theme.json, or ~/.config/callisto/theme.json.
// Empty when neither the variable nor HOME is set.
[[nodiscard]] std::filesystem::path theme_config_path();

// A built-in name, a path to JSON, or empty. Empty reads theme.json when that file exists.
[[nodiscard]] Theme load_theme(std::string_view spec);
