//
// File containing tests for edit_file.
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

#include <fstream>
#include <sstream>

static std::string file_body(const std::filesystem::path &path) {
    std::ifstream in(path);
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

/**
 * A patch of only added lines creates the file, including a missing parent directory.
 */
static int test_tool_edit_file_creates() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "nested" / "out.txt";

    const auto tool = Tools::create_edit_file_tool();
    const std::string patch = "--- /dev/null\n+++ b/out.txt\n@@ -0,0 +1,2 @@\n+hello\n+world\n";
    const auto result = tool.execute({{"path", path.string()}, {"patch", patch}});
    assertTrue(result.find("--- /dev/null\n") != std::string::npos);
    assertTrue(result.find("+hello\n") != std::string::npos);
    assertTrue(result.find("+world\n") != std::string::npos);
    assertEquals("hello\nworld\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * The same creation works without a /dev/null header.
 */
static int test_tool_edit_file_creates_without_header() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "out.txt";

    const auto tool = Tools::create_edit_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"patch", "@@ -0,0 +1,1 @@\n+hello\n"}});
    assertTrue(result.find("+hello\n") != std::string::npos);
    assertEquals("hello\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A new-file patch does not replace a file that is already there.
 */
static int test_tool_edit_file_create_refuses_existing() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "out.txt";
    write_test_file(path, "old\n");

    const auto tool = Tools::create_edit_file_tool();
    const std::string patch = "--- /dev/null\n+++ b/out.txt\n@@ -0,0 +1,1 @@\n+new\n";
    const auto result = tool.execute({{"path", path.string()}, {"patch", patch}});
    assertEquals("error: " + path.string() + " already exists; match its current lines", result);
    assertEquals("old\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A missing path or patch argument is an error.
 */
static int test_tool_edit_file_missing_arguments() {
    const auto tool = Tools::create_edit_file_tool();
    assertEquals("error: missing required string argument 'path'", tool.execute({{"patch", "+hello\n"}}));
    assertEquals("error: missing required string argument 'patch'", tool.execute({{"path", "out.txt"}}));
    assertEquals("error: missing required string argument 'patch'",
                 tool.execute({{"path", "out.txt"}, {"patch", 1}}));
    return EXIT_SUCCESS;
}

/**
 * A unified diff replaces the matching lines. Stale @@ numbers still apply when the context matches.
 */
static int test_tool_edit_file_replace() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\nbeta\ngamma\ndelta\n");

    const auto tool = Tools::create_edit_file_tool();
    const std::string patch = "--- a/note.txt\n+++ b/note.txt\n@@ -40,3 +40,3 @@\n alpha\n-beta\n+BETA\n gamma\n";
    const auto result = tool.execute({{"path", path.string()}, {"patch", patch}});
    const std::string diff = "--- " + path.string() + "\n+++ " + path.string() +
                             "\n@@ -1,4 +1,4 @@\n alpha\n-beta\n+BETA\n gamma\n delta\n";
    assertEquals(diff, result);
    assertEquals("alpha\nBETA\ngamma\ndelta\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * One patch can grow, insert, and delete lines in separate hunks.
 */
static int test_tool_edit_file_resize_and_insert() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\nbeta\ngamma\ndelta\n");

    const auto tool = Tools::create_edit_file_tool();
    const std::string grown = "@@ -1,3 +1,4 @@\n alpha\n-beta\n+B1\n+B2\n gamma\n";
    const auto grew = tool.execute({{"path", path.string()}, {"patch", grown}});
    assertTrue(grew.find("-beta\n") != std::string::npos);
    assertTrue(grew.find("+B1\n") != std::string::npos);
    assertTrue(grew.find("+B2\n") != std::string::npos);
    assertEquals("alpha\nB1\nB2\ngamma\ndelta\n", file_body(path));

    const std::string inserted = "@@ -1,2 +1,3 @@\n alpha\n+NEW\n B1\n";
    const auto added = tool.execute({{"path", path.string()}, {"patch", inserted}});
    assertTrue(added.find("+NEW\n") != std::string::npos);
    assertEquals("alpha\nNEW\nB1\nB2\ngamma\ndelta\n", file_body(path));

    const std::string removed = "@@ -3,2 +3,0 @@\n-B1\n-B2\n";
    const auto deleted = tool.execute({{"path", path.string()}, {"patch", removed}});
    assertTrue(deleted.find("-B1\n") != std::string::npos);
    assertTrue(deleted.find("-B2\n") != std::string::npos);
    assertEquals("alpha\nNEW\ngamma\ndelta\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A zero-length old side appends at that line, the same way git apply does.
 */
static int test_tool_edit_file_append() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\n");

    const auto tool = Tools::create_edit_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"patch", "@@ -1,0 +2,1 @@\n+beta\n"}});
    assertTrue(result.find("+beta\n") != std::string::npos);
    assertEquals("alpha\nbeta\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A hunk that repeats the file is rejected and the file stays unchanged.
 */
static int test_tool_edit_file_unchanged() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\nbeta\n");

    const auto tool = Tools::create_edit_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"patch", "@@ -1,2 +1,2 @@\n alpha\n beta\n"}});
    assertEquals("error: patch does not change the file", result);
    assertEquals("alpha\nbeta\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A repeated block is not guessed. The file stays unchanged when the hunk fits in two places.
 */
static int test_tool_edit_file_ambiguous() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\nbeta\nm\nn\nalpha\nbeta\n");

    const auto tool = Tools::create_edit_file_tool();
    const std::string patch = "@@ -3,2 +3,2 @@\n alpha\n-beta\n+BETA\n";
    const auto result = tool.execute({{"path", path.string()}, {"patch", patch}});
    assertEquals("error: hunk 1 matches more than one place in " + path.string() + "; add a few unchanged lines",
                 result);
    assertEquals("alpha\nbeta\nm\nn\nalpha\nbeta\n", file_body(path));
    return EXIT_SUCCESS;
}

/**
 * A missing file, a hunk that does not match, and a patch for another path are errors.
 */
static int test_tool_edit_file_errors() {
    const auto dir = make_temp_dir("edit-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\n");
    const auto missing = dir / "missing.txt";

    const auto tool = Tools::create_edit_file_tool();
    assertEquals("error: " + missing.string() + " does not exist; a new file patch contains only + lines",
                 tool.execute({{"path", missing.string()}, {"patch", "@@ -1 +1 @@\n-a\n+b\n"}}));
    assertEquals("error: hunk 1 did not match " + path.string(),
                 tool.execute({{"path", path.string()}, {"patch", "@@ -1,1 +1,1 @@\n-missing\n+x\n"}}));
    assertEquals("error: patch has no hunks", tool.execute({{"path", path.string()}, {"patch", "not a diff"}}));
    assertEquals("error: missing required string argument 'patch'", tool.execute({{"path", path.string()}}));
    assertEquals("error: " + path.string() + " already exists; match its current lines",
                 tool.execute({{"path", path.string()},
                               {"patch", "--- /dev/null\n+++ b/note.txt\n@@ -0,0 +1,1 @@\n+alpha\n"}}));
    assertEquals("error: patch is for b/other.txt, not " + path.string(),
                 tool.execute({{"path", path.string()},
                               {"patch", "--- a/other.txt\n+++ b/other.txt\n@@ -1 +1 @@\n-alpha\n+beta\n"}}));
    assertTrue(tool.execute({{"path", path.string()}, {"patch", "@@ -1 +1 @@\nno prefix\n"}})
                   .find("does not start with a space") != std::string::npos);
    assertEquals("alpha\n", file_body(path));
    return EXIT_SUCCESS;
}

int test_tool_write_file() {
    RUN_TEST(test_tool_edit_file_creates);
    RUN_TEST(test_tool_edit_file_creates_without_header);
    RUN_TEST(test_tool_edit_file_create_refuses_existing);
    RUN_TEST(test_tool_edit_file_missing_arguments);
    RUN_TEST(test_tool_edit_file_replace);
    RUN_TEST(test_tool_edit_file_resize_and_insert);
    RUN_TEST(test_tool_edit_file_append);
    RUN_TEST(test_tool_edit_file_unchanged);
    RUN_TEST(test_tool_edit_file_ambiguous);
    RUN_TEST(test_tool_edit_file_errors);
    return EXIT_SUCCESS;
}
