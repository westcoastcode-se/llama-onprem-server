#include "cli/tui/theme.hpp"
#include "../tests.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{

int test_default_theme_uses_the_old_palette()
{
    const Theme theme = theme_builtin("default");
    assertEquals(std::string("default"), theme.name);
    assertEquals(8, theme.thinking.palette);
    assertEquals(7, theme.thinking_text.palette);
    assertEquals(3, theme.tool.palette);
    assertEquals(6, theme.you.palette);
    assertEquals(15, theme.text.palette);
    assertEquals(1, theme.meter_hot.palette);
    assertTrue(theme.border);
    assertTrue(theme.rounded);
    assertTrue(theme.bold);
    assertEquals(std::string("\033[90m"), theme.thinking.ansi());
    assertEquals(std::string("\033[33m"), theme.tool.ansi());
    return EXIT_SUCCESS;
}

int test_ink_theme_is_rgb()
{
    const Theme theme = theme_builtin("ink");
    assertEquals(std::string("ink"), theme.name);
    assertEquals(-1, theme.tool.palette);
    assertEquals(224, static_cast<int>(theme.tool.r));
    assertEquals(164, static_cast<int>(theme.tool.g));
    assertEquals(90, static_cast<int>(theme.tool.b));
    assertEquals(std::string("\033[38;2;224;164;90m"), theme.tool.ansi());
    return EXIT_SUCCESS;
}

int test_named_themes_exist()
{
    const Theme nord = theme_builtin("nord");
    const Theme forest = theme_builtin("forest");
    assertEquals(std::string("nord"), nord.name);
    assertTrue(nord.border);
    assertTrue(!nord.rounded);
    assertEquals(std::string("forest"), forest.name);
    assertTrue(!forest.border);
    assertTrue(!forest.bold);
    assertEquals(std::string("ember"), theme_builtin("ember").name);
    assertTrue(!theme_builtin("ink").bold);
    const Theme ink = theme_builtin("ink");
    assertEquals(212, static_cast<int>(ink.thinking_text.r));
    assertEquals(138, static_cast<int>(ink.thinking.r));
    return EXIT_SUCCESS;
}

int test_unknown_theme_is_rejected()
{
    bool threw = false;
    try
    {
        (void)theme_builtin("nope");
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    assertTrue(threw);
    return EXIT_SUCCESS;
}

int test_theme_json_overrides_one_color()
{
    const Theme theme = theme_from_json(R"({"theme":"default","you":"#0f0","name":"mine"})");
    assertEquals(std::string("mine"), theme.name);
    assertEquals(-1, theme.you.palette);
    assertEquals(0, static_cast<int>(theme.you.r));
    assertEquals(255, static_cast<int>(theme.you.g));
    assertEquals(0, static_cast<int>(theme.you.b));
    assertEquals(3, theme.tool.palette);
    const Theme shape = theme_from_json(R"({"border":false,"rounded":false,"bold":false})");
    assertTrue(!shape.border);
    assertTrue(!shape.rounded);
    assertTrue(!shape.bold);
    bool bad_flag = false;
    try
    {
        (void)theme_from_json(R"({"bold":"yes"})");
    }
    catch (const std::runtime_error &)
    {
        bad_flag = true;
    }
    assertTrue(bad_flag);
    return EXIT_SUCCESS;
}

int test_theme_json_accepts_palette_names()
{
    const Theme theme = theme_from_json(R"({"tool":" yellow ","meter_hot":"redlight"})");
    assertEquals(3, theme.tool.palette);
    assertEquals(9, theme.meter_hot.palette);
    return EXIT_SUCCESS;
}

int test_theme_json_rejects_unknown_key_and_color()
{
    bool unknown_key = false;
    try
    {
        (void)theme_from_json(R"({"background":"#000"})");
    }
    catch (const std::runtime_error &)
    {
        unknown_key = true;
    }
    assertTrue(unknown_key);

    bool unknown_color = false;
    try
    {
        (void)theme_from_json(R"({"you":"orange"})");
    }
    catch (const std::runtime_error &)
    {
        unknown_color = true;
    }
    assertTrue(unknown_color);

    bool bad_hex = false;
    try
    {
        (void)theme_from_json(R"({"you":"#12"})");
    }
    catch (const std::runtime_error &)
    {
        bad_hex = true;
    }
    assertTrue(bad_hex);
    return EXIT_SUCCESS;
}

int test_load_theme_file_and_missing_config()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "callisto-theme-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / ".config" / "callisto");
    const std::filesystem::path file = dir / "custom.json";
    {
        std::ofstream out(file);
        out << R"({"theme":"ink","you":"#fff"})";
    }
    const Theme from_path = load_theme(file.string());
    assertEquals(std::string("ink"), from_path.name);
    assertEquals(-1, from_path.you.palette);
    assertEquals(255, static_cast<int>(from_path.you.r));

    const char *previous = std::getenv("HOME");
    const std::string saved = previous == nullptr ? std::string() : previous;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string saved_xdg = xdg == nullptr ? std::string() : xdg;
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", dir.c_str(), 1);
    const Theme missing = load_theme("");
    {
        std::ofstream out(dir / ".config" / "callisto" / "theme.json");
        out << R"({"tool":"#abc"})";
    }
    const Theme from_home = load_theme("");
    if (previous == nullptr)
    {
        unsetenv("HOME");
    }
    else
    {
        setenv("HOME", saved.c_str(), 1);
    }
    if (xdg == nullptr)
    {
        unsetenv("XDG_CONFIG_HOME");
    }
    else
    {
        setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
    }
    std::filesystem::remove_all(dir);

    assertEquals(std::string("default"), missing.name);
    assertEquals(8, missing.thinking.palette);
    assertEquals(-1, from_home.tool.palette);
    assertEquals(170, static_cast<int>(from_home.tool.r));
    assertEquals(187, static_cast<int>(from_home.tool.g));
    assertEquals(204, static_cast<int>(from_home.tool.b));
    return EXIT_SUCCESS;
}

} // namespace

int test_theme()
{
    RUN_TEST(test_default_theme_uses_the_old_palette);
    RUN_TEST(test_ink_theme_is_rgb);
    RUN_TEST(test_named_themes_exist);
    RUN_TEST(test_unknown_theme_is_rejected);
    RUN_TEST(test_theme_json_overrides_one_color);
    RUN_TEST(test_theme_json_accepts_palette_names);
    RUN_TEST(test_theme_json_rejects_unknown_key_and_color);
    RUN_TEST(test_load_theme_file_and_missing_config);
    return EXIT_SUCCESS;
}
