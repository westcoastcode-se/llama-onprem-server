//
// File containing tests for the file_search tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * A filename substring match is returned as a path.
 */
static int test_tool_file_search_match() {
    const auto dir = make_temp_dir("file-search");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "needle.txt";
    write_test_file(path, "x");
    write_test_file(dir / "other.txt", "y");

    const auto tool = Tools::create_file_search_tool();
    const auto result = tool.execute({{"pattern", "needle"}, {"path", dir.string()}});
    assertEquals(path.string() + "\n", result);
    return EXIT_SUCCESS;
}

/**
 * No matching name is reported as no matches.
 */
static int test_tool_file_search_none() {
    const auto dir = make_temp_dir("file-search");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "other.txt", "y");

    const auto tool = Tools::create_file_search_tool();
    const auto result = tool.execute({{"pattern", "needle"}, {"path", dir.string()}});
    assertEquals("no matching files found", result);
    return EXIT_SUCCESS;
}

/**
 * A missing pattern argument is an error.
 */
static int test_tool_file_search_error_missing_pattern() {
    const auto tool = Tools::create_file_search_tool();
    const auto result = tool.execute(nlohmann::json::object());
    assertEquals("error: missing required string argument 'pattern'", result);
    return EXIT_SUCCESS;
}

/**
 * A pattern that is not a string is the same error as a missing pattern.
 */
static int test_tool_file_search_error_invalid_pattern() {
    const auto tool = Tools::create_file_search_tool();
    const auto result = tool.execute({{"pattern", 1}});
    assertEquals("error: missing required string argument 'pattern'", result);
    return EXIT_SUCCESS;
}

/**
 * Run all file_search tests
 */
int test_tool_file_search() {
    RUN_TEST(test_tool_file_search_match);
    RUN_TEST(test_tool_file_search_none);
    RUN_TEST(test_tool_file_search_error_missing_pattern);
    RUN_TEST(test_tool_file_search_error_invalid_pattern);
    return EXIT_SUCCESS;
}
