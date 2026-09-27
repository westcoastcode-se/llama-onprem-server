//
// File containing tests for the list_directory tool
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../../tests.hpp"

/**
 * A directory with one file lists that file and its size.
 */
static int test_tool_list_directory_file() {
    const auto dir = make_temp_dir("list-dir");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "hello.txt", "hello");

    const auto tool = Tools::create_list_directory_tool();
    const auto result = tool.execute({{"path", dir.string()}});
    assertEquals("[FILE] hello.txt (5 bytes)\n", result);
    return EXIT_SUCCESS;
}

/**
 * An empty directory is reported as empty.
 */
static int test_tool_list_directory_empty() {
    const auto dir = make_temp_dir("list-dir");
    defer(std::filesystem::remove_all(dir));

    const auto tool = Tools::create_list_directory_tool();
    const auto result = tool.execute({{"path", dir.string()}});
    assertEquals("(empty directory)", result);
    return EXIT_SUCCESS;
}

/**
 * A missing directory is an error.
 */
static int test_tool_list_directory_missing() {
    const auto dir = make_temp_dir("list-dir");
    defer(std::filesystem::remove_all(dir));
    const auto missing = dir / "nope";

    const auto tool = Tools::create_list_directory_tool();
    const auto result = tool.execute({{"path", missing.string()}});
    assertEquals("error: directory does not exist: " + missing.string(), result);
    return EXIT_SUCCESS;
}

/**
 * A file path is not a directory.
 */
static int test_tool_list_directory_not_a_directory() {
    const auto dir = make_temp_dir("list-dir");
    defer(std::filesystem::remove_all(dir));
    const auto path = dir / "hello.txt";
    write_test_file(path, "hello");

    const auto tool = Tools::create_list_directory_tool();
    const auto result = tool.execute({{"path", path.string()}});
    assertEquals("error: path is not a directory: " + path.string(), result);
    return EXIT_SUCCESS;
}

/**
 * Run all list_directory tests
 */
/**
 * Directories are listed before files, and each group is sorted by name.
 */
static int test_tool_list_directory_order() {
    const auto dir = make_temp_dir("list-dir");
    defer(std::filesystem::remove_all(dir));
    write_test_file(dir / "b.txt", "bb");
    write_test_file(dir / "a.txt", "a");
    std::filesystem::create_directory(dir / "zdir");

    const auto tool = Tools::create_list_directory_tool();
    const auto result = tool.execute({{"path", dir.string()}});
    assertEquals("[DIR]  zdir\n[FILE] a.txt (1 bytes)\n[FILE] b.txt (2 bytes)\n", result);
    return EXIT_SUCCESS;
}

int test_tool_list_directory() {
    RUN_TEST(test_tool_list_directory_file);
    RUN_TEST(test_tool_list_directory_order);
    RUN_TEST(test_tool_list_directory_empty);
    RUN_TEST(test_tool_list_directory_missing);
    RUN_TEST(test_tool_list_directory_not_a_directory);
    return EXIT_SUCCESS;
}
