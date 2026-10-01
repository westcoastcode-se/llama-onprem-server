//
// File containing tests for tool approval, allow-lists, think stripping, and tool-call parsing
//

#include "common/defer.hpp"
#include "common/tools.hpp"
#include "../tests.hpp"

#include <sstream>

/**
 * Auto-approve, a wildcard, and a listed name allow a tool. Anything else does not.
 */
static int test_is_tool_allowed() {
    const std::vector<std::string> allowed = {"read_file"};
    assertTrue(is_tool_allowed("Read_File", false, allowed));
    assertTrue(is_tool_allowed("edit_file", true, allowed));
    assertTrue(!is_tool_allowed("edit_file", false, allowed));

    const std::vector<std::string> all = {"*"};
    assertTrue(is_tool_allowed("edit_file", false, all));
    return EXIT_SUCCESS;
}

/**
 * string_view_trim drops whitespace on both ends.
 */
static int test_string_view_trim() {
    assertEquals("hi", string_view_trim(" \n\thi\t "));
    assertTrue(string_view_trim(" \n\t").empty());
    return EXIT_SUCCESS;
}

/**
 * run_tool finds a tool by name and by alias, and reports an unknown name.
 */
static int test_run_tool_name_and_alias() {
    const Tool tool{
        .name = "echo",
        .description = "",
        .schema_doc = "",
        .execute = [](const nlohmann::json &args) { return args.value("text", std::string{}); },
        .aliases = {"say"},
    };
    const std::span<const Tool> tools(&tool, 1);
    assertEquals("hi", run_tool(tools, "echo", {{"text", "hi"}}));
    assertEquals("hi", run_tool(tools, "say", {{"text", "hi"}}));
    assertEquals("error: unknown tool 'nope'", run_tool(tools, "nope", nlohmann::json::object()));
    return EXIT_SUCCESS;
}

/**
 * An exception from a tool becomes an error string.
 */
static int test_run_tool_exception() {
    const Tool tool{
        .name = "echo",
        .description = "",
        .schema_doc = "",
        .execute = [](const nlohmann::json &) -> std::string { throw std::runtime_error("boom"); },
    };
    const std::span<const Tool> tools(&tool, 1);
    assertEquals("error executing tool 'echo': boom", run_tool(tools, "echo", nlohmann::json::object()));
    return EXIT_SUCCESS;
}

/**
 * The base registry is the file and web tools. Sub-agents are optional.
 */
static int test_registered_tools() {
    const auto base = get_base_tools();
    assertEquals(9, static_cast<int>(base.size()));
    assertEquals("execute_command", base.front().name);
    assertEquals("web_search", base.back().name);

    const auto with_sub = get_registered_tools(true, [](std::string_view, bool) { return std::string("ok"); });
    assertEquals(10, static_cast<int>(with_sub.size()));
    assertEquals("sub_agent", with_sub.back().name);
    assertEquals(11, static_cast<int>(with_sub.back().aliases.size()));
    assertEquals(std::string("subagent"), with_sub.back().aliases.front());
    assertEquals(std::string("run_tasks"), with_sub.back().aliases.back());
    assertTrue(Tools::is_subagent_name("sub_agent"));
    assertTrue(Tools::is_subagent_name("spawn_subagent"));
    assertTrue(!Tools::is_subagent_name("read_file"));

    const auto without = get_registered_tools(false);
    assertEquals(9, static_cast<int>(without.size()));
    return EXIT_SUCCESS;
}

/**
 * An empty instructions file is ignored. A later candidate is not used when an earlier one has text.
 */
static int test_load_ai_instructions() {
    const auto dir = make_temp_dir("prompt");
    defer(std::filesystem::remove_all(dir));
    assertEquals("", load_agents_markdown(dir.string()));

    write_test_file(dir / ".agents" / "AGENTS.md", "from callisto\n");
    write_test_file(dir / ".agents" / "AGENTS.md", "from agents\n");
    assertEquals("", load_agents_markdown(dir.string()));

    write_test_file(dir / "AGENTS.md", "  \n");
    assertEquals("", load_agents_markdown(dir.string()));

    write_test_file(dir / "AGENTS.md", "from the first file\n");
    assertEquals("from the first file", load_agents_markdown(dir.string()));
    return EXIT_SUCCESS;
}

/**
 * Run all tool-policy tests
 */
int test_tool_policy() {
    RUN_TEST(test_is_tool_allowed);
    RUN_TEST(test_string_view_trim);
    RUN_TEST(test_run_tool_name_and_alias);
    RUN_TEST(test_run_tool_exception);
    RUN_TEST(test_registered_tools);
    RUN_TEST(test_load_ai_instructions);
    return EXIT_SUCCESS;
}
