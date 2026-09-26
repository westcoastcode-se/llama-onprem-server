//
// File containing tests for the write_file tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

#include <fstream>
#include <sstream>

/**
 * Write a file and report the byte count.
 */
static int test_tool_write_file_creates() {
    const auto dir = make_temp_dir("write-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "nested" / "out.txt";

    const auto tool = Tools::create_write_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"content", "hello"}});
    assertEquals("success: wrote 5 bytes to " + path.string(), result);

    std::ifstream in(path);
    std::ostringstream body;
    body << in.rdbuf();
    assertEquals("hello", body.str());
    return EXIT_SUCCESS;
}

/**
 * A later write replaces the previous contents.
 */
static int test_tool_write_file_overwrites() {
    const auto dir = make_temp_dir("write-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "out.txt";
    write_test_file(path, "old");

    const auto tool = Tools::create_write_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"content", "new"}});
    assertEquals("success: wrote 3 bytes to " + path.string(), result);

    std::ifstream in(path);
    std::ostringstream body;
    body << in.rdbuf();
    assertEquals("new", body.str());
    return EXIT_SUCCESS;
}

/**
 * A missing path argument is an error.
 */
static int test_tool_write_file_error_missing_path() {
    const auto tool = Tools::create_write_file_tool();
    const auto result = tool.execute({{"content", "hello"}});
    assertEquals("error: missing required string argument 'path'", result);
    return EXIT_SUCCESS;
}

/**
 * A missing content argument is an error.
 */
static int test_tool_write_file_error_missing_content() {
    const auto tool = Tools::create_write_file_tool();
    const auto result = tool.execute({{"path", "out.txt"}});
    assertEquals("error: missing required string argument 'content'", result);
    return EXIT_SUCCESS;
}

/**
 * A content value that is not a string is the same error as a missing content.
 */
static int test_tool_write_file_error_invalid_content() {
    const auto tool = Tools::create_write_file_tool();
    const auto result = tool.execute({{"path", "out.txt"}, {"content", 1}});
    assertEquals("error: missing required string argument 'content'", result);
    return EXIT_SUCCESS;
}

/**
 * Run all write_file tests
 */
int test_tool_write_file() {
    RUN_TEST(test_tool_write_file_creates);
    RUN_TEST(test_tool_write_file_overwrites);
    RUN_TEST(test_tool_write_file_error_missing_path);
    RUN_TEST(test_tool_write_file_error_missing_content);
    RUN_TEST(test_tool_write_file_error_invalid_content);
    return EXIT_SUCCESS;
}
