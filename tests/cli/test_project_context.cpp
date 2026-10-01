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
    const auto skills = list_skills(dir);
    assertEquals(static_cast<std::size_t>(2), skills.size());
    assertEquals(std::string("build"), skills[0].name);
    assertEquals(std::string("Build the debug binaries."), skills[0].summary);
    assertEquals(std::string(".agents/skills/build/SKILL.md"), skills[0].path);
    assertTrue(skills[0].summary.find("must stay out") == std::string::npos);
    return 0;
}

} // namespace

int test_project_context()
{
    RUN_TEST(test_list_skills_uses_front_matter);
    return 0;
}
