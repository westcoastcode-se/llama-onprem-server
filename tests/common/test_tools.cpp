//
// File containing tests for generic tools functions
//

#include "../tests.hpp"
#include "common/tools.hpp"

/**
 * Each tool chooses the text after its name on the collapsed line.
 * An alias uses the same text.
 */
int test_tool_present_comes_from_the_tool()
{
    const nlohmann::json search_args = {{"path", "."}, {"query", "alpha|beta"}};
    const Tool *search = find_tool(get_base_tools(), "grep");
    assertTrue(search != nullptr && static_cast<bool>(search->present));
    assertEquals("alpha|beta", search->present(search_args));

    const Tool *files = find_tool(get_base_tools(), "file_search");
    assertTrue(files != nullptr && static_cast<bool>(files->present));
    assertEquals("note.hpp", files->present({{"path", "src"}, {"pattern", "note.hpp"}}));

    const Tool *command = find_tool(get_base_tools(), "execute_command");
    assertTrue(command != nullptr && static_cast<bool>(command->present));
    assertEquals("ls", command->present({{"command", "ls"}}));

    const Tool *page = find_tool(get_base_tools(), "web_fetch");
    assertTrue(page != nullptr && static_cast<bool>(page->present));
    assertEquals("http://example", page->present({{"url", "http://example"}}));

    const auto sub = Tools::create_subagent_tool(nullptr);
    assertTrue(static_cast<bool>(sub.present));
    assertEquals("read the map", sub.present({{"task", "read the map"}}));
    return EXIT_SUCCESS;
}

/**
 * Run all generic tools functions tests
 */
int test_tools()
{
    RUN_TEST(test_tool_present_comes_from_the_tool);
    return EXIT_SUCCESS;
}
