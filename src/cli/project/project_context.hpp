#pragma once

#include <filesystem>
#include <string>
#include <vector>

/**
 * Information about a skill found in the project
 */
struct SkillNote
{
    // Skill name
    std::string name;
    // The skill summary
    std::string summary;
    // Path to the SKILL.md file
    std::string path;
};

/**
 * @return Directory for agent files under the project root, such as skills
 */
static constexpr std::filesystem::path get_intelligence_root_dir()
{
    return {".agents"};
}

/**
 * Project skills, relative to the project root.
 * Pattern: .agents/skills/<name>/SKILL.md
 */
static constexpr std::filesystem::path skills_directory()
{
    return get_intelligence_root_dir() / std::filesystem::path{"skills"};
}

/**
 * Skills under the project root at .agents/skills/<name>/SKILL.md.
 *
 * Only the skill name, summary and path to the skills file are included.
 *
 * @param cwd Project root
 * @return A list of all skills
 */
[[nodiscard]] std::vector<SkillNote> list_skills(const std::filesystem::path &cwd);

/**
 * Try to list all programming languages used in this project. Those languages should be a hint on what skills
 * the project should load and supply to the AI agent. Basically it should collect all file extensions found in the
 * project and create a summary on languages used.
 *
 * @param cwd Directory
 * @return A list of all programming languages used in this project
 */
[[nodiscard]] std::vector<std::string> list_extensions(const std::filesystem::path &cwd);