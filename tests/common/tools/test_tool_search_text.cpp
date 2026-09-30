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
    assertEquals(path.string() + "\n1: Hello Needle\n", result);
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
    assertEquals(cpp.string() + "\n1: hit\n", result);
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
    assertEquals(keep.string() + "\n1: secret\n", result);
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
    assertEquals(text.string() + "\n1: secret\n", result);
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
    const auto result = tool.execute(
        {{"query", "secret"}, {"path", path.string()}, {"max_matches", 1}, {"before", 0}, {"context", 0}});
    assertEquals(path.string() + "\n1: secret one\n\n... [matches truncated]", result);
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
 * method regex matches a pattern. An invalid pattern is an error, and an unknown method is rejected.
 */
static int test_tool_search_text_regex() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "Hello Needle\n");

    const auto tool = Tools::create_search_text_tool();
    const auto found = tool.execute({{"query", "n.edle"}, {"path", path.string()}, {"method", "regex"}, {"before", 0}, {"context", 0}});
    assertEquals(path.string() + "\n1: Hello Needle\n", found);

    const auto bad = tool.execute({{"query", "("}, {"path", path.string()}, {"method", "regex"}});
    assertTrue(bad.starts_with("error: invalid regular expression:"));

    const auto unknown = tool.execute({{"query", "Hello"}, {"path", path.string()}, {"method", "fuzzy"}});
    assertEquals("error: method must be text, extended, or regex", unknown);
    return EXIT_SUCCESS;
}

/**
 * Plain text treats a pipe as a character. extended matches either alternative.
 */
static int test_tool_search_text_pipe_is_literal_unless_extended() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "alpha\nalpha|beta\nbeta\n");

    const auto tool = Tools::create_search_text_tool();
    const auto literal = tool.execute({{"query", "alpha|beta"}, {"path", path.string()}, {"before", 0}, {"context", 0}});
    assertEquals(path.string() + "\n2: alpha|beta\n", literal);

    const auto either = tool.execute(
        {{"query", "alpha|beta"}, {"path", path.string()}, {"method", "extended"}, {"before", 0}, {"context", 0}});
    assertEquals(path.string() + "\n1: alpha\n2: alpha|beta\n3: beta\n", either);

    const auto first = dir / "one.txt";
    const auto second = dir / "two.txt";
    write_test_file(first, "alpha\n");
    write_test_file(second, "beta\n");
    const auto across = tool.execute(
        {{"query", "alpha|beta"}, {"path", dir.string()}, {"method", "extended"}, {"before", 0}, {"context", 0}, {"file_pattern", ".txt"}});
    assertTrue(across.find(first.string() + "\n1: alpha\n") != std::string::npos);
    assertTrue(across.find(second.string() + "\n1: beta\n") != std::string::npos);
    assertEquals(across.find(first.string()), across.rfind(first.string()));
    assertEquals(across.find(second.string()), across.rfind(second.string()));
    return EXIT_SUCCESS;
}

/**
 * Spaces around the pipe are not part of either extended alternative.
 */
static int test_tool_search_text_pipe_trims_alternatives() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "Alpha\n");

    const auto tool = Tools::create_search_text_tool();
    const auto result = tool.execute(
        {{"query", " alpha | missing "}, {"path", path.string()}, {"method", "extended"}, {"before", 0}, {"context", 0}});
    assertEquals(path.string() + "\n1: Alpha\n", result);
    return EXIT_SUCCESS;
}

/**
 * before lists earlier lines. context lists lines after the hit.
 * A line inside two hit windows is listed once.
 */
static int test_tool_search_text_before_and_context() {
    const auto dir = make_temp_dir("search-text");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "note.txt";
    write_test_file(path, "one\ntwo\nthree\nfour\nhit\nafter\ntrail\n");

    const auto tool = Tools::create_search_text_tool();
    const auto around = tool.execute({{"query", "hit"}, {"path", path.string()}, {"before", 1}, {"context", 1}});
    assertEquals(path.string() + "\n4: four\n5: hit\n6: after\n", around);

    const auto defaults = tool.execute({{"query", "hit"}, {"path", path.string()}, {"context", 0}});
    assertEquals(path.string() + "\n2: two\n3: three\n4: four\n5: hit\n", defaults);

    write_test_file(path, "a\nb\nhit\nc\nhit\nd\n");
    const auto once = tool.execute({{"query", "hit"}, {"path", path.string()}, {"before", 3}, {"context", 0}});
    assertEquals(path.string() + "\n1: a\n2: b\n3: hit\n4: c\n5: hit\n", once);
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
    assertEquals(keep.string() + "\n1: secret\n", result);
    return EXIT_SUCCESS;
}

int test_tool_search_text() {
    RUN_TEST(test_tool_search_text_match);
    RUN_TEST(test_tool_search_text_pipe_is_literal_unless_extended);
    RUN_TEST(test_tool_search_text_pipe_trims_alternatives);
    RUN_TEST(test_tool_search_text_case_sensitive);
    RUN_TEST(test_tool_search_text_file_pattern);
    RUN_TEST(test_tool_search_text_skips_git);
    RUN_TEST(test_tool_search_text_skips_vendors);
    RUN_TEST(test_tool_search_text_before_and_context);
    RUN_TEST(test_tool_search_text_skips_binary);
    RUN_TEST(test_tool_search_text_max_matches);
    RUN_TEST(test_tool_search_text_error_missing_query);
    RUN_TEST(test_tool_search_text_error_missing_path);
    RUN_TEST(test_tool_search_text_regex);
    return EXIT_SUCCESS;
}
