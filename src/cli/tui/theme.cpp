#include "cli/tui/theme.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <stdexcept>

namespace
{

ThemeColor palette(int index)
{
    ThemeColor color;
    color.palette = index;
    return color;
}

ThemeColor rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    ThemeColor color;
    color.r = r;
    color.g = g;
    color.b = b;
    return color;
}

Theme make_default()
{
    Theme theme;
    theme.name = "default";
    theme.thinking = palette(8);
    theme.thinking_text = palette(7);
    theme.surface = palette(8);
    theme.tool = palette(3);
    theme.tool_text = palette(7);
    theme.you = palette(6);
    theme.note = palette(7);
    theme.text = palette(15);
    theme.prompt = palette(3);
    theme.meter = palette(7);
    theme.meter_warn = palette(3);
    theme.meter_hot = palette(1);
    theme.border = true;
    theme.rounded = true;
    theme.bold = true;
    return theme;
}

Theme make_ink()
{
    Theme theme;
    theme.name = "ink";
    theme.thinking = rgb(138, 129, 117);
    theme.thinking_text = rgb(212, 203, 184);
    theme.surface = rgb(42, 38, 34);
    theme.tool = rgb(224, 164, 90);
    theme.tool_text = rgb(200, 191, 176);
    theme.you = rgb(126, 184, 201);
    theme.note = rgb(138, 129, 117);
    theme.text = rgb(243, 239, 230);
    theme.prompt = rgb(224, 164, 90);
    theme.meter = rgb(138, 129, 117);
    theme.meter_warn = rgb(224, 164, 90);
    theme.meter_hot = rgb(212, 101, 74);
    theme.border = true;
    theme.rounded = true;
    theme.bold = false;
    return theme;
}

Theme make_nord()
{
    Theme theme;
    theme.name = "nord";
    theme.thinking = rgb(76, 86, 106);
    theme.thinking_text = rgb(216, 222, 233);
    theme.surface = rgb(46, 52, 64);
    theme.tool = rgb(235, 203, 139);
    theme.tool_text = rgb(229, 233, 240);
    theme.you = rgb(136, 192, 208);
    theme.note = rgb(129, 161, 193);
    theme.text = rgb(236, 239, 244);
    theme.prompt = rgb(235, 203, 139);
    theme.meter = rgb(216, 222, 233);
    theme.meter_warn = rgb(235, 203, 139);
    theme.meter_hot = rgb(191, 97, 106);
    theme.border = true;
    theme.rounded = false;
    theme.bold = true;
    return theme;
}

Theme make_forest()
{
    Theme theme;
    theme.name = "forest";
    theme.thinking = rgb(107, 143, 113);
    theme.thinking_text = rgb(197, 213, 197);
    theme.surface = rgb(28, 36, 28);
    theme.tool = rgb(143, 188, 143);
    theme.tool_text = rgb(213, 230, 213);
    theme.you = rgb(126, 200, 195);
    theme.note = rgb(138, 163, 138);
    theme.text = rgb(231, 240, 231);
    theme.prompt = rgb(163, 217, 165);
    theme.meter = rgb(138, 163, 138);
    theme.meter_warn = rgb(212, 195, 106);
    theme.meter_hot = rgb(212, 106, 106);
    theme.border = false;
    theme.rounded = true;
    theme.bold = false;
    return theme;
}

Theme make_ember()
{
    Theme theme;
    theme.name = "ember";
    theme.thinking = rgb(166, 124, 101);
    theme.thinking_text = rgb(230, 210, 196);
    theme.surface = rgb(42, 33, 28);
    theme.tool = rgb(224, 138, 79);
    theme.tool_text = rgb(240, 210, 192);
    theme.you = rgb(230, 176, 137);
    theme.note = rgb(168, 144, 128);
    theme.text = rgb(246, 235, 227);
    theme.prompt = rgb(224, 138, 79);
    theme.meter = rgb(168, 144, 128);
    theme.meter_warn = rgb(224, 176, 79);
    theme.meter_hot = rgb(212, 92, 74);
    theme.border = true;
    theme.rounded = true;
    theme.bold = true;
    return theme;
}

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

std::string lower_copy(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

ThemeColor parse_hex(std::string_view text)
{
    auto byte = [](int hi, int lo) {
        return static_cast<std::uint8_t>(hi * 16 + lo);
    };
    if (text.size() == 3)
    {
        const int r = hex_nibble(text[0]);
        const int g = hex_nibble(text[1]);
        const int b = hex_nibble(text[2]);
        if (r < 0 || g < 0 || b < 0)
        {
            throw std::runtime_error("color must be #rgb or #rrggbb");
        }
        return rgb(byte(r, r), byte(g, g), byte(b, b));
    }
    if (text.size() == 6)
    {
        const int r1 = hex_nibble(text[0]);
        const int r2 = hex_nibble(text[1]);
        const int g1 = hex_nibble(text[2]);
        const int g2 = hex_nibble(text[3]);
        const int b1 = hex_nibble(text[4]);
        const int b2 = hex_nibble(text[5]);
        if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0)
        {
            throw std::runtime_error("color must be #rgb or #rrggbb");
        }
        return rgb(byte(r1, r2), byte(g1, g2), byte(b1, b2));
    }
    throw std::runtime_error("color must be #rgb or #rrggbb");
}

ThemeColor parse_color(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.front() == '#')
    {
        return parse_hex(text.substr(1));
    }
    const std::string name = lower_copy(text);
    if (name == "black")
        return palette(0);
    if (name == "red")
        return palette(1);
    if (name == "green")
        return palette(2);
    if (name == "yellow")
        return palette(3);
    if (name == "blue")
        return palette(4);
    if (name == "magenta")
        return palette(5);
    if (name == "cyan")
        return palette(6);
    if (name == "gray" || name == "graylight")
        return palette(7);
    if (name == "graydark")
        return palette(8);
    if (name == "redlight")
        return palette(9);
    if (name == "greenlight")
        return palette(10);
    if (name == "yellowlight")
        return palette(11);
    if (name == "bluelight")
        return palette(12);
    if (name == "magentalight")
        return palette(13);
    if (name == "cyanlight")
        return palette(14);
    if (name == "white")
        return palette(15);
    throw std::runtime_error("unknown color '" + std::string(text) + "'");
}

bool known_key(std::string_view key)
{
    return key == "theme" || key == "name" || key == "thinking" || key == "thinking_text" || key == "surface" ||
           key == "tool" || key == "tool_text" || key == "you" || key == "note" || key == "text" || key == "prompt" ||
           key == "meter" || key == "meter_warn" || key == "meter_hot" || key == "border" || key == "rounded" ||
           key == "bold";
}

void apply_bool(const nlohmann::json &body, const char *key, bool &slot)
{
    if (!body.contains(key))
    {
        return;
    }
    if (!body.at(key).is_boolean())
    {
        throw std::runtime_error(std::string(key) + " must be true or false");
    }
    slot = body.at(key).get<bool>();
}

void apply_color(const nlohmann::json &body, const char *key, ThemeColor &slot)
{
    if (!body.contains(key))
    {
        return;
    }
    if (!body.at(key).is_string())
    {
        throw std::runtime_error(std::string(key) + " must be a color string");
    }
    slot = parse_color(body.at(key).get<std::string>());
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

std::string ThemeColor::ansi() const
{
    if (palette >= 0 && palette <= 7)
    {
        return "\033[" + std::to_string(30 + palette) + "m";
    }
    if (palette >= 8 && palette <= 15)
    {
        return "\033[" + std::to_string(82 + palette) + "m";
    }
    return std::format("\033[38;2;{};{};{}m", r, g, b);
}

Theme theme_builtin(std::string_view name)
{
    if (name == "default")
    {
        return make_default();
    }
    if (name == "ink")
    {
        return make_ink();
    }
    if (name == "nord")
    {
        return make_nord();
    }
    if (name == "forest")
    {
        return make_forest();
    }
    if (name == "ember")
    {
        return make_ember();
    }
    throw std::runtime_error(
        "unknown theme '" + std::string(name) + "'. Built-in themes are default, ink, nord, forest, and ember");
}

Theme theme_from_json(std::string_view text)
{
    nlohmann::json body;
    try
    {
        body = nlohmann::json::parse(text);
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::string("invalid theme JSON: ") + error.what());
    }
    if (!body.is_object())
    {
        throw std::runtime_error("theme JSON must be an object");
    }
    for (const auto &item : body.items())
    {
        if (!known_key(item.key()))
        {
            throw std::runtime_error("unknown theme key '" + item.key() + "'");
        }
    }
    std::string base = "default";
    if (body.contains("theme"))
    {
        if (!body.at("theme").is_string())
        {
            throw std::runtime_error("theme must be a string");
        }
        base = body.at("theme").get<std::string>();
    }
    Theme theme = theme_builtin(base);
    if (body.contains("name"))
    {
        if (!body.at("name").is_string())
        {
            throw std::runtime_error("name must be a string");
        }
        theme.name = body.at("name").get<std::string>();
    }
    apply_color(body, "thinking", theme.thinking);
    apply_color(body, "thinking_text", theme.thinking_text);
    apply_color(body, "surface", theme.surface);
    apply_color(body, "tool", theme.tool);
    apply_color(body, "tool_text", theme.tool_text);
    apply_color(body, "you", theme.you);
    apply_color(body, "note", theme.note);
    apply_color(body, "text", theme.text);
    apply_color(body, "prompt", theme.prompt);
    apply_color(body, "meter", theme.meter);
    apply_color(body, "meter_warn", theme.meter_warn);
    apply_color(body, "meter_hot", theme.meter_hot);
    apply_bool(body, "border", theme.border);
    apply_bool(body, "rounded", theme.rounded);
    apply_bool(body, "bold", theme.bold);
    return theme;
}

std::filesystem::path theme_config_path()
{
    const char *home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        return {};
    }
    return std::filesystem::path(home) / ".config" / "callisto" / "theme.json";
}

Theme load_theme(std::string_view spec)
{
    if (!spec.empty())
    {
        const bool file = spec.find('/') != std::string_view::npos || spec.ends_with(".json");
        if (file)
        {
            return theme_from_json(read_file(spec));
        }
        return theme_builtin(spec);
    }
    const std::filesystem::path path = theme_config_path();
    if (path.empty() || !std::filesystem::exists(path))
    {
        return theme_builtin("default");
    }
    return theme_from_json(read_file(path));
}
