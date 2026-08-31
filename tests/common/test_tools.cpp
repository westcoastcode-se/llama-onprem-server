//
// File containing tests for generic tools functions
//

#include "common/tools.hpp"
#include "../tests.hpp"

/**
 * Verify the strip_response_stream function, which basically gives us
 * The important parts from the response into usable blocks, such as the think text, the tool_calls, questions etc.
 */
int test_strip_response_stream() {
    const auto block = ResponseBlocks::from_text("<think>Lorem Ipsum</think>");
    assertEquals("Lorem Ipsum", block.thinking);
    return EXIT_SUCCESS;
}

/**
 * Run all generic tools functions tests
 */
void test_tools() {
    RUN_TEST(test_strip_response_stream);
}
