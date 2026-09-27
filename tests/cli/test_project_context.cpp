#include "cli/project_context.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <fstream>

namespace
{

int test_list_skills_uses_front_matter()
{
    const auto dir = make_temp_dir("skills");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / ".callisto" / "skills" / "build" / "SKILL.md",
                    "---\nname: build\ndescription: Build the debug binaries.\n---\n"
                    "Run cmake, then the tests. This sentence must stay out of the index.\n");
    const auto skills = list_skills(dir);
    assertEquals(static_cast<std::size_t>(1), skills.size());
    assertEquals(std::string("build"), skills[0].name);
    assertEquals(std::string("Build the debug binaries."), skills[0].summary);
    assertEquals(std::string(".callisto/skills/build/SKILL.md"), skills[0].path);
    assertTrue(skills[0].summary.find("must stay out") == std::string::npos);
    return 0;
}

int test_refresh_project_map_lists_layout_and_skips_vendors()
{
    const auto dir = make_temp_dir("map");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "CMakeLists.txt", "add_executable(callisto_cli src/cli.cpp)\nadd_test(NAME tests COMMAND tests)\n");
    write_test_file(dir / "src" / "cli" / "main.cpp", "int main(){return 0;}\n");
    write_test_file(dir / "vendors" / "secret.txt", "do not index\n");
    assertTrue(refresh_project_map(dir, true));
    assertTrue(!refresh_project_map(dir, false));

    std::ifstream in(dir / ".callisto" / "map.md");
    std::string map((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assertTrue(map.find("callisto_cli") != std::string::npos);
    assertTrue(map.find("src/") != std::string::npos);
    assertTrue(map.find("cli/") != std::string::npos);
    assertTrue(map.find("secret") == std::string::npos);
    assertTrue(map.find("vendors/") == std::string::npos);
    return 0;
}

} // namespace

int test_project_context()
{
    RUN_TEST(test_list_skills_uses_front_matter);
    RUN_TEST(test_refresh_project_map_lists_layout_and_skips_vendors);
    return 0;
}
