//
// File containing tests for generic tools functions
//

#include "common/tools.hpp"
#include "../tests.hpp"

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_one_line() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum</think>");
    assertTrue((block.flags & ResponseBlocks::thinking_bit) == ResponseBlocks::thinking_bit);
    assertTrue((block.flags & ResponseBlocks::thinking_done_bit) == ResponseBlocks::thinking_done_bit);
    assertEquals("Lorem Ipsum", block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_multiline_line() {
    const auto block = ResponseBlocks::from_text("<think>\nLorem Ipsum\n</think>\n");
    assertTrue((block.flags & ResponseBlocks::thinking_bit) == ResponseBlocks::thinking_bit);
    assertTrue((block.flags & ResponseBlocks::thinking_done_bit) == ResponseBlocks::thinking_done_bit);
    assertEquals("Lorem Ipsum", block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_broken_end() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum</th");
    assertTrue((block.flags & ResponseBlocks::thinking_bit) == ResponseBlocks::thinking_bit);
    assertTrue((block.flags & ResponseBlocks::thinking_done_bit) == 0);
    assertEquals("Lorem Ipsum", block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_missing_end_tag() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum<");
    assertTrue((block.flags & ResponseBlocks::thinking_bit) == ResponseBlocks::thinking_bit);
    assertTrue((block.flags & ResponseBlocks::thinking_done_bit) == 0);
    assertEquals("Lorem Ipsum", block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_empty() {
    const auto block = ResponseBlocks::from_text("<think></think>");
    assertTrue((block.flags & ResponseBlocks::thinking_bit) == ResponseBlocks::thinking_bit);
    assertTrue((block.flags & ResponseBlocks::thinking_done_bit) == ResponseBlocks::thinking_done_bit);
    assertEquals(std::string_view(""), block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_and_tool_call_one_line() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum</think><tool_call>{}</tool_call>");
    assertEquals("Lorem Ipsum", block.thinking);
    assertEquals(1, block.tool_calls.size());
    assertTrue(block.tool_calls[0].is_done);
    assertEquals("{}", block.tool_calls[0].value);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_and_tool_call_multi_line() {
    const auto block = ResponseBlocks::from_text("<think>\nLorem Ipsum\n</think>\n<tool_call>\n{}\n</tool_call>\n");
    assertEquals("Lorem Ipsum", block.thinking);
    assertEquals(1, block.tool_calls.size());
    assertTrue(block.tool_calls[0].is_done);
    assertEquals("{}", block.tool_calls[0].value);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_and_multiple_tool_call_one_line() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum</think><tool_call>{a}</tool_call><tool_call>{b}</tool_call>");
    assertEquals("Lorem Ipsum", block.thinking);
    assertEquals(2, block.tool_calls.size());
    assertTrue(block.tool_calls[0].is_done);
    assertEquals("{a}", block.tool_calls[0].value);
    assertTrue(block.tool_calls[1].is_done);
    assertEquals("{b}", block.tool_calls[1].value);
    return EXIT_SUCCESS;
}

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream_think_and_multiple_tool_call_multi_line() {
    const auto block = ResponseBlocks::from_text("<think>\nLorem Ipsum\n</think>\n<tool_call>\n{a}\n</tool_call><tool_call>\n{b}\n</tool_call>\n");
    assertEquals("Lorem Ipsum", block.thinking);
    assertEquals(2, block.tool_calls.size());
    assertTrue(block.tool_calls[0].is_done);
    assertEquals("{a}", block.tool_calls[0].value);
    assertTrue(block.tool_calls[1].is_done);
    assertEquals("{b}", block.tool_calls[1].value);
    return EXIT_SUCCESS;
}

/**
 * Plain assistant text has no blocks.
 */
int test_strip_response_stream_plain_text() {
    const auto block = ResponseBlocks::from_text("Just an answer.");
    assertEquals(0, block.flags);
    assertTrue(block.thinking.empty());
    assertTrue(block.tool_calls.empty());
    return EXIT_SUCCESS;
}

/**
 * A tool call that has not closed yet stays open.
 */
int test_strip_response_stream_open_tool_call() {
    const auto block = ResponseBlocks::from_text("<tool_call>{\"name\":\"read_file\"");
    assertEquals(1, block.tool_calls.size());
    assertTrue(!block.tool_calls[0].is_done);
    assertEquals("{\"name\":\"read_file\"", block.tool_calls[0].value);
    return EXIT_SUCCESS;
}
/**
 * Each tool chooses the text after its name on the collapsed line.
 * An alias uses the same text.
 */
int test_tool_present_comes_from_the_tool() {
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
int test_tools() {
    RUN_TEST(test_strip_response_stream_think_one_line);
    RUN_TEST(test_strip_response_stream_think_multiline_line);
    RUN_TEST(test_strip_response_stream_think_broken_end);
    RUN_TEST(test_strip_response_stream_think_missing_end_tag);
    RUN_TEST(test_strip_response_stream_think_empty);

    RUN_TEST(test_strip_response_stream_think_and_tool_call_one_line);
    RUN_TEST(test_strip_response_stream_think_and_tool_call_multi_line);
    RUN_TEST(test_strip_response_stream_think_and_multiple_tool_call_one_line);
    RUN_TEST(test_strip_response_stream_think_and_multiple_tool_call_multi_line);
    RUN_TEST(test_strip_response_stream_plain_text);
    RUN_TEST(test_strip_response_stream_open_tool_call);
    RUN_TEST(test_tool_present_comes_from_the_tool);
    return EXIT_SUCCESS;
}
