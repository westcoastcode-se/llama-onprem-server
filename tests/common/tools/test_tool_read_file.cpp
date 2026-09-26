//
// File containing tests for the read_file tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * Read a small file with line numbers.
 */
static int test_tool_read_file_lines() {
    const auto dir = make_temp_dir("read-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "hello\nworld\n");

    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", path.string()}});
    assertEquals("1: hello\n2: world\n", result);
    return EXIT_SUCCESS;
}

/**
 * offset and limit select a window of lines.
 */
static int test_tool_read_file_offset_and_limit() {
    const auto dir = make_temp_dir("read-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "one\ntwo\nthree\n");

    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"offset", 2}, {"limit", 1}});
    assertEquals("2: two\n", result);
    return EXIT_SUCCESS;
}

/**
 * An empty file is reported as empty.
 */
static int test_tool_read_file_empty() {
    const auto dir = make_temp_dir("read-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "empty.txt";
    write_test_file(path, "");

    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", path.string()}});
    assertEquals("(empty file)", result);
    return EXIT_SUCCESS;
}

/**
 * An offset past the last line is an error.
 */
static int test_tool_read_file_offset_past_end() {
    const auto dir = make_temp_dir("read-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "only\n");

    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", path.string()}, {"offset", 5}});
    assertEquals("error: offset 5 is beyond file length (1 lines)", result);
    return EXIT_SUCCESS;
}

/**
 * A missing path argument is an error.
 */
static int test_tool_read_file_error_missing_path() {
    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute(nlohmann::json::object());
    assertEquals("error: missing required string argument 'path'", result);
    return EXIT_SUCCESS;
}

/**
 * A path that is not a string is the same error as a missing path.
 */
static int test_tool_read_file_error_invalid_path() {
    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", 100}});
    assertEquals("error: missing required string argument 'path'", result);
    return EXIT_SUCCESS;
}

/**
 * A path that cannot be opened is an error.
 */
static int test_tool_read_file_error_missing_file() {
    const auto dir = make_temp_dir("read-file");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "missing.txt";

    const auto tool = Tools::create_read_file_tool();
    const auto result = tool.execute({{"path", path.string()}});
    assertEquals("error: could not open file '" + path.string() + "'", result);
    return EXIT_SUCCESS;
}

/**
 * Run all read_file tests
 */
int test_tool_read_file() {
    RUN_TEST(test_tool_read_file_lines);
    RUN_TEST(test_tool_read_file_offset_and_limit);
    RUN_TEST(test_tool_read_file_empty);
    RUN_TEST(test_tool_read_file_offset_past_end);
    RUN_TEST(test_tool_read_file_error_missing_path);
    RUN_TEST(test_tool_read_file_error_invalid_path);
    RUN_TEST(test_tool_read_file_error_missing_file);
    return EXIT_SUCCESS;
}
