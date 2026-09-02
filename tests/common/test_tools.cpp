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
 * Run all generic tools functions tests
 */
void test_tools() {
    RUN_TEST(test_strip_response_stream_think_one_line);
    RUN_TEST(test_strip_response_stream_think_multiline_line);
    RUN_TEST(test_strip_response_stream_think_broken_end);
    RUN_TEST(test_strip_response_stream_think_missing_end_tag);
    RUN_TEST(test_strip_response_stream_think_empty);

    RUN_TEST(test_strip_response_stream_think_and_tool_call_one_line);
    RUN_TEST(test_strip_response_stream_think_and_tool_call_multi_line);
}
