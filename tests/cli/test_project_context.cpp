#include "cli/project/project_context.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <fstream>

namespace
{

[[nodiscard]] std::string read_map(const std::filesystem::path &dir)
{
    std::ifstream in(dir / ".callisto" / "map.md");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct MapParts
{
    std::string layout;
    std::string skip;
};

[[nodiscard]] MapParts split_map(const std::string &map)
{
    MapParts parts;
    const auto layout_at = map.find("## Layout\n");
    const auto skip_at = map.find("## Skip\n");
    if (layout_at == std::string::npos || skip_at == std::string::npos || skip_at < layout_at)
    {
        return parts;
    }
    parts.layout = map.substr(layout_at, skip_at - layout_at);
    parts.skip = map.substr(skip_at);
    return parts;
}

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

int test_refresh_project_map_lists_layout_and_vendor_names()
{
    const auto dir = make_temp_dir("map");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "CMakeLists.txt", "add_executable(callisto_cli src/cli.cpp)\nadd_test(NAME tests COMMAND tests)\n");
    write_test_file(dir / "src" / "cli" / "main.cpp", "int main(){return 0;}\n");
    write_test_file(dir / "vendors" / "cli11" / "CLI11.hpp", "vendor header\n");
    write_test_file(dir / "vendors" / "llama.cpp" / "src" / "hidden_vendor.cpp", "do not index\n");
    write_test_file(dir / "node_modules" / "leftpad" / "index.js", "skip\n");
    write_test_file(dir / ".gitignore", "node_modules\n");
    assertTrue(refresh_project_map(dir, true));
    assertTrue(!refresh_project_map(dir, false));

    const std::string map = read_map(dir);
    const MapParts parts = split_map(map);
    assertTrue(map.find("Kind: CMake") != std::string::npos);
    assertTrue(map.find("callisto_cli") != std::string::npos);
    assertTrue(map.find("src/") != std::string::npos);
    assertTrue(map.find("cli/") != std::string::npos);
    assertTrue(parts.layout.find("vendors/") != std::string::npos);
    assertTrue(parts.layout.find("cli11/") != std::string::npos);
    assertTrue(parts.layout.find("llama.cpp/") != std::string::npos);
    assertTrue(map.find("CLI11.hpp") == std::string::npos);
    assertTrue(map.find("hidden_vendor") == std::string::npos);
    assertTrue(parts.layout.find("leftpad") == std::string::npos);
    assertTrue(parts.layout.find("node_modules") == std::string::npos);
    assertTrue(parts.skip.find("node_modules") != std::string::npos);
    assertTrue(parts.skip.find("vendors") == std::string::npos);
    return 0;
}

int test_refresh_project_map_without_gitignore_lists_directories()
{
    const auto dir = make_temp_dir("map-no-ignore");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "node_modules" / "leftpad" / "index.js", "skip\n");
    write_test_file(dir / "build" / "a.o", "obj\n");
    write_test_file(dir / "src" / "main.cpp", "int main(){return 0;}\n");
    assertTrue(refresh_project_map(dir, true));

    const std::string map = read_map(dir);
    const MapParts parts = split_map(map);
    assertTrue(parts.layout.find("node_modules/") != std::string::npos);
    assertTrue(parts.layout.find("leftpad/") != std::string::npos);
    assertTrue(parts.layout.find("build/") != std::string::npos);
    assertTrue(parts.layout.find("a.o") != std::string::npos);
    assertTrue(parts.layout.find("src/") != std::string::npos);
    assertTrue(parts.skip.find("No .gitignore.") != std::string::npos);
    assertTrue(map.find("cmake-build-debug") == std::string::npos);
    return 0;
}

int test_refresh_project_map_uses_gitignore()
{
    const auto dir = make_temp_dir("map-ignore");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / ".gitignore",
                    "# comment\n"
                    "\n"
                    "build/\n"
                    "*.log\n"
                    "!notes.log\n"
                    "src/*.o\n"
                    "secret.txt\n"
                    "nest/**\n"
                    "cmake-build-*\n"
                    "cache-[ab]\n"
                    "temp.?\n"
                    "node_modules/\n"
                    "!node_modules/keep.js\n");
    write_test_file(dir / "build" / "out.txt", "obj\n");
    write_test_file(dir / "keep" / "build", "not a directory\n");
    write_test_file(dir / "notes.log", "keep\n");
    write_test_file(dir / "other.log", "drop\n");
    write_test_file(dir / "src" / "main.cpp", "int main(){return 0;}\n");
    write_test_file(dir / "src" / "main.o", "obj\n");
    write_test_file(dir / "src" / "secret.txt", "drop\n");
    write_test_file(dir / "lib" / "a.o", "obj\n");
    write_test_file(dir / "secret.txt", "drop\n");
    write_test_file(dir / "nest" / "visible.txt", "drop\n");
    write_test_file(dir / "cmake-build-debug" / "x", "drop\n");
    write_test_file(dir / "cache-a", "drop\n");
    write_test_file(dir / "cache-c", "keep\n");
    write_test_file(dir / "temp.x", "drop\n");
    write_test_file(dir / "temp.xy", "keep\n");
    write_test_file(dir / "node_modules" / "keep.js", "drop\n");
    assertTrue(refresh_project_map(dir, true));

    const std::string map = read_map(dir);
    const MapParts parts = split_map(map);
    assertTrue(!parts.layout.empty());
    assertTrue(parts.layout.find("- keep/: build\n") != std::string::npos);
    assertTrue(parts.layout.find("- lib/: a.o\n") != std::string::npos);
    assertTrue(parts.layout.find("- nest/\n") != std::string::npos);
    assertTrue(parts.layout.find("- notes.log\n") != std::string::npos);
    assertTrue(parts.layout.find("- src/: main.cpp\n") != std::string::npos);
    assertTrue(parts.layout.find("- cache-c\n") != std::string::npos);
    assertTrue(parts.layout.find("- temp.xy\n") != std::string::npos);
    assertTrue(parts.layout.find("build/") == std::string::npos);
    assertTrue(parts.layout.find("other.log") == std::string::npos);
    assertTrue(parts.layout.find("main.o") == std::string::npos);
    assertTrue(parts.layout.find("secret.txt") == std::string::npos);
    assertTrue(parts.layout.find("visible.txt") == std::string::npos);
    assertTrue(parts.layout.find("cmake-build-debug") == std::string::npos);
    assertTrue(parts.layout.find("cache-a") == std::string::npos);
    assertTrue(parts.layout.find("- temp.x\n") == std::string::npos);
    assertTrue(parts.layout.find("node_modules") == std::string::npos);
    assertTrue(parts.layout.find("keep.js") == std::string::npos);
    assertTrue(parts.skip.find("# comment") == std::string::npos);
    assertTrue(parts.skip.find("build/") != std::string::npos);
    assertTrue(parts.skip.find("!notes.log") != std::string::npos);
    assertTrue(parts.skip.find("src/*.o") != std::string::npos);
    assertTrue(parts.skip.find("nest/**") != std::string::npos);
    assertTrue(parts.skip.find("!node_modules/keep.js") != std::string::npos);
    return 0;
}

int test_refresh_project_map_describes_node()
{
    const auto dir = make_temp_dir("node-map");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "package.json",
                    "{\n"
                    "  \"name\": \"widget\",\n"
                    "  \"scripts\": { \"test\": \"vitest\", \"build\": \"tsc\" }\n"
                    "}\n");
    assertTrue(refresh_project_map(dir, true));
    const std::string map = read_map(dir);
    assertTrue(map.find("Kind: Node.js") != std::string::npos);
    assertTrue(map.find("Package: widget") != std::string::npos);
    assertTrue(map.find("Scripts: build, test") != std::string::npos);
    assertTrue(map.find("Tests: npm test") != std::string::npos);
    assertTrue(map.find("Kind: CMake") == std::string::npos);
    assertTrue(map.find("See the build files") == std::string::npos);
    return 0;
}

int test_refresh_project_map_describes_rust()
{
    const auto dir = make_temp_dir("rust-map");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "Cargo.toml",
                    "[package]\n"
                    "name = \"widget\"\n"
                    "\n"
                    "[[bin]]\n"
                    "name = \"widget\"\n"
                    "\n"
                    "[workspace]\n"
                    "members = [\n"
                    "  \"crates/core\",\n"
                    "]\n");
    assertTrue(refresh_project_map(dir, true));
    const std::string map = read_map(dir);
    assertTrue(map.find("Kind: Rust") != std::string::npos);
    assertTrue(map.find("Package: widget") != std::string::npos);
    assertTrue(map.find("Bins: widget") != std::string::npos);
    assertTrue(map.find("Workspace: crates/core") != std::string::npos);
    assertTrue(map.find("Tests: cargo test") != std::string::npos);
    assertTrue(map.find("Kind: CMake") == std::string::npos);
    return 0;
}

int test_refresh_project_map_describes_maven()
{
    const auto dir = make_temp_dir("maven-map");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "pom.xml",
                    "<project>\n"
                    "  <parent><artifactId>parent-bom</artifactId></parent>\n"
                    "  <artifactId>widget</artifactId>\n"
                    "  <dependencies><dependency><artifactId>junit</artifactId></dependency></dependencies>\n"
                    "  <modules>\n"
                    "    <module>app</module>\n"
                    "    <module>lib</module>\n"
                    "  </modules>\n"
                    "</project>\n");
    assertTrue(refresh_project_map(dir, true));
    const std::string map = read_map(dir);
    assertTrue(map.find("Kind: Maven") != std::string::npos);
    assertTrue(map.find("Artifact: widget") != std::string::npos);
    assertTrue(map.find("Modules: app, lib") != std::string::npos);
    assertTrue(map.find("Tests: mvn test") != std::string::npos);
    assertTrue(map.find("parent-bom") == std::string::npos);
    assertTrue(map.find("junit") == std::string::npos);
    assertTrue(map.find("Kind: CMake") == std::string::npos);
    return 0;
}

} // namespace

int test_project_context()
{
    RUN_TEST(test_list_skills_uses_front_matter);
    RUN_TEST(test_refresh_project_map_lists_layout_and_vendor_names);
    RUN_TEST(test_refresh_project_map_without_gitignore_lists_directories);
    RUN_TEST(test_refresh_project_map_uses_gitignore);
    RUN_TEST(test_refresh_project_map_describes_node);
    RUN_TEST(test_refresh_project_map_describes_rust);
    RUN_TEST(test_refresh_project_map_describes_maven);
    return 0;
}
