#include "cli/project/project_context.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

namespace
{

int test_list_skills_uses_front_matter()
{
    const auto dir = make_temp_dir("skills");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / ".agents" / "skills" / "build" / "SKILL.md",
                    "---\nname: build\ndescription: Build the debug binaries.\n---\n"
                    "Run cmake, then the tests. This sentence must stay out of the index.\n");
    write_test_file(dir / ".agents" / "skills" / "old" / "SKILL.md",
                    "---\nname: old\ndescription: The previous skills directory.\n---\n");
    const auto skills = list_skills(dir, dir / "missing-user-skills");
    assertEquals(static_cast<std::size_t>(2), skills.size());
    assertEquals(std::string("build"), skills[0].name);
    assertEquals(std::string("Build the debug binaries."), skills[0].summary);
    assertEquals(std::string(".agents/skills/build/SKILL.md"), skills[0].path);
    assertTrue(skills[0].summary.find("must stay out") == std::string::npos);
    return 0;
}

/**
 * ~/.agents/skills is listed too. A project skill with the same name stays.
 */
int test_list_skills_includes_home()
{
    const auto project = make_temp_dir("skills-project");
    const auto home = make_temp_dir("skills-home");
    defer(std::filesystem::remove_all(project));
    defer(std::filesystem::remove_all(home));
    write_test_file(project / ".agents" / "skills" / "build" / "SKILL.md",
                    "---\nname: build\ndescription: Project build.\n---\n");
    write_test_file(home / "review" / "SKILL.md", "---\nname: review\ndescription: Review a diff.\n---\n");
    write_test_file(home / "build" / "SKILL.md", "---\nname: build\ndescription: Home build.\n---\n");

    const auto skills = list_skills(project, home);
    assertEquals(static_cast<std::size_t>(2), skills.size());
    assertEquals(std::string("build"), skills[0].name);
    assertEquals(std::string("Project build."), skills[0].summary);
    assertEquals(std::string(".agents/skills/build/SKILL.md"), skills[0].path);
    assertEquals(std::string("review"), skills[1].name);
    assertEquals(std::string("Review a diff."), skills[1].summary);
    const auto review = std::filesystem::absolute(home / "review" / "SKILL.md").generic_string();
    assertEquals(review, skills[1].path);
    return 0;
}

} // namespace

int test_project_context()
{
    RUN_TEST(test_list_skills_uses_front_matter);
    RUN_TEST(test_list_skills_includes_home);
    return 0;
}
