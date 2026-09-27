#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct SkillNote
{
    std::string name;
    std::string summary;
    std::string path;
};

// One line per skill under .callisto/skills/<name>/SKILL.md. The body is not included.
[[nodiscard]] std::vector<SkillNote> list_skills(const std::filesystem::path &cwd);

// Write .callisto/map.md from the tree. Rewrites when git HEAD changes, or when force is set.
// Returns true when the file was written.
[[nodiscard]] bool refresh_project_map(const std::filesystem::path &cwd, bool force);
