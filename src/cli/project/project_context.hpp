#pragma once

#include <filesystem>
#include <string>
#include <vector>

/**
 * One skill the prompt can point the model at.
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
 * Skills under the project root at .agents/skills/<name>/SKILL.md, then skills in
 * user_skills when that directory exists. user_skills is the skills directory
 * itself, such as ~/.agents/skills. An empty path adds nothing. A user skill
 * whose name is already listed from the project is skipped.
 *
 * Only the skill name, summary, and path are included. A project path is relative
 * to cwd. A user path is absolute, so read_file can open it.
 *
 * @param cwd Project root
 * @param user_skills User skills directory, or an empty path
 * @return The skills, sorted by name
 */
[[nodiscard]] std::vector<SkillNote> list_skills(const std::filesystem::path &cwd,
                                                 const std::filesystem::path &user_skills);

/**
 * Project skills plus $HOME/.agents/skills/<name>/SKILL.md.
 * A missing HOME adds no user skills.
 *
 * @param cwd Project root
 * @return The skills, sorted by name
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