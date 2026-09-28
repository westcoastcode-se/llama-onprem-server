//
// File containing tests for the search_text tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * A case-insensitive query reports file, line, and the matching line.
 */
static int test_tool_search_text_match() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "Hello Needle\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "needle"}, {"path", path.string()}});
    assertEquals(path.string() + ":1: Hello Needle\n", result);
    return EXIT_SUCCESS;
}

/**
 * case_sensitive leaves a different-case line unmatched.
 */
static int test_tool_search_text_case_sensitive() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "Hello Needle\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "needle"}, {"path", path.string()}, {"case_sensitive", true}});
    assertEquals("no matches found for query 'needle'", result);
    return EXIT_SUCCESS;
}

/**
 * file_pattern keeps only names that contain the filter.
 */
static int test_tool_search_text_file_pattern() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto cpp = dir / "a.cpp";
    write_test_file(cpp, "hit\n");
    write_test_file(dir / "b.txt", "hit\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "hit"}, {"path", dir.string()}, {"file_pattern", ".cpp"}});
    assertEquals(cpp.string() + ":1: hit\n", result);
    return EXIT_SUCCESS;
}

/**
 * VCS directories are not searched.
 */
static int test_tool_search_text_skips_git() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto keep = dir / "keep.txt";
    write_test_file(keep, "secret\n");
    write_test_file(dir / ".git" / "note.txt", "secret\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "secret"}, {"path", dir.string()}});
    assertEquals(keep.string() + ":1: secret\n", result);
    return EXIT_SUCCESS;
}

/**
 * A file with a NUL byte is skipped.
 */
static int test_tool_search_text_skips_binary() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto text = dir / "note.txt";
    write_test_file(text, "secret\n");
    write_test_file(dir / "bin.dat", std::string("secret\0more", 11));

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "secret"}, {"path", dir.string()}});
    assertEquals(text.string() + ":1: secret\n", result);
    return EXIT_SUCCESS;
}

/**
 * max_matches stops the report and marks it truncated.
 */
static int test_tool_search_text_max_matches() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "secret one\nsecret two\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "secret"}, {"path", path.string()}, {"max_matches", 1}, {"context", 0}});
    assertEquals(path.string() + ":1: secret one\n\n... [matches truncated]", result);
    return EXIT_SUCCESS;
}

/**
 * A missing query is an error.
 */
static int test_tool_search_text_error_missing_query() {
    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute(nlohmann::json::object());
    assertEquals("error: missing required string argument 'query' or 'pattern'", result);
    return EXIT_SUCCESS;
}

/**
 * A path that does not exist is an error.
 */
static int test_tool_search_text_error_missing_path() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto missing = dir / "missing";

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "secret"}, {"path", missing.string()}});
    assertEquals("error: path '" + missing.string() + "' does not exist", result);
    return EXIT_SUCCESS;
}

/**
 * An invalid regular expression is an error.
 */
static int test_tool_search_text_error_bad_regex() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "hello\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "("}, {"path", path.string()}, {"is_regex", true}});
    assertTrue(result.starts_with("error: invalid regular expression:"));
    return EXIT_SUCCESS;
}

/**
 * Run all search_text tests
 */
/**
 * text1|text2 matches either alternative, including across files.
 */
static int test_tool_search_text_pipe_is_either() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto first = dir / "one.txt";
    const auto second = dir / "two.txt";
    write_test_file(first, "see alpha here\n");
    write_test_file(second, "see beta there\nnope\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "alpha|beta"}, {"path", dir.string()}, {"context", 0}});
    assertTrue(result.find(first.string() + ":1: see alpha here") != std::string::npos);
    assertTrue(result.find(second.string() + ":1: see beta there") != std::string::npos);
    assertTrue(result.find("nope") == std::string::npos);
    return EXIT_SUCCESS;
}

/**
 * Spaces around the pipe are not part of either alternative.
 */
static int test_tool_search_text_pipe_trims_alternatives() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "Alpha\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", " alpha | missing "}, {"path", path.string()}, {"context", 0}});
    assertEquals(path.string() + ":1: Alpha\n", result);
    return EXIT_SUCCESS;
}

/**
 * context includes the lines around a hit.
 */
static int test_tool_search_text_context() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "before\nhit\nafter\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "hit"}, {"path", path.string()}, {"context", 1}});
    assertEquals(path.string() + ":1: before\n" + path.string() + ":2: hit\n" + path.string() + ":3: after\n", result);
    return EXIT_SUCCESS;
}

/**
 * vendors is not searched when walking a project root.
 */
static int test_tool_search_text_skips_vendors() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto keep = dir / "keep.txt";
    write_test_file(keep, "secret\n");
    write_test_file(dir / "vendors" / "note.txt", "secret\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute({{"query", "secret"}, {"path", dir.string()}, {"context", 0}});
    assertEquals(keep.string() + ":1: secret\n", result);
    return EXIT_SUCCESS;
}

int test_tool_search_text() {
    RUN_TEST(test_tool_search_text_match);
    RUN_TEST(test_tool_search_text_pipe_is_either);
    RUN_TEST(test_tool_search_text_pipe_trims_alternatives);
    RUN_TEST(test_tool_search_text_case_sensitive);
    RUN_TEST(test_tool_search_text_file_pattern);
    RUN_TEST(test_tool_search_text_skips_git);
    RUN_TEST(test_tool_search_text_skips_vendors);
    RUN_TEST(test_tool_search_text_context);
    RUN_TEST(test_tool_search_text_skips_binary);
    RUN_TEST(test_tool_search_text_max_matches);
    RUN_TEST(test_tool_search_text_error_missing_query);
    RUN_TEST(test_tool_search_text_error_missing_path);
    RUN_TEST(test_tool_search_text_error_bad_regex);
    return EXIT_SUCCESS;
}
