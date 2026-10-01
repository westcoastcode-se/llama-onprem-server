#pragma once

#include "common/tools.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Shared by the project-map pieces. Not part of the public CLI API.
namespace project_detail
{

inline constexpr int kChildCap = 12;

[[nodiscard]] inline std::string read_text(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in)
    {
        return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

[[nodiscard]] inline std::string trim_copy(std::string_view text)
{
    return std::string(string_view_trim(text));
}

inline void write_text(const std::filesystem::path &path, std::string_view text)
{
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

[[nodiscard]] std::string git_head(const std::filesystem::path &cwd);

[[nodiscard]] std::string build_hints(const std::filesystem::path &cwd);

struct IgnoreRule
{
    std::vector<std::string> parts;
    std::string base;
    bool negate = false;
    bool directory_only = false;
    bool anchored = false;
};

struct GitIgnore
{
    bool present = false;
    std::vector<IgnoreRule> rules;
    std::vector<std::string> patterns;
};

[[nodiscard]] GitIgnore load_gitignore(const std::filesystem::path &cwd);

// A later "!" rule cannot bring back a file when a parent directory is already ignored.
[[nodiscard]] bool ignored(const GitIgnore &ignore, std::string_view relative, bool directory);

[[nodiscard]] std::string skip_note(const GitIgnore &ignore);

} // namespace project_detail
