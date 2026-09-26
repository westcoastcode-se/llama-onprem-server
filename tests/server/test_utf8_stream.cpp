//
// File containing tests for the UTF-8 stream buffer
//

#include "server/llm/utf8_stream.hpp"
#include "../tests.hpp"

/**
 * A complete code point is released. A split code point waits for the rest.
 */
static int test_utf8_stream_holds_partial() {
    Utf8Util::StreamBuffer buffer;
    assertEquals("", buffer.process(""));
    assertEquals("A", buffer.process("A"));
    assertEquals("", buffer.process("\xC3"));
    assertTrue(!buffer.empty());
    assertEquals("å", buffer.process("\xA5"));
    assertTrue(buffer.empty());
    return EXIT_SUCCESS;
}

/**
 * flush drops a code point that never finished and leaves the buffer empty.
 */
static int test_utf8_stream_flush() {
    Utf8Util::StreamBuffer partial;
    assertEquals("", partial.process("\xC3"));
    assertEquals("", partial.flush());
    assertTrue(partial.empty());
    assertEquals("Z", partial.process("Z"));

    Utf8Util::StreamBuffer tail;
    assertEquals("ok", tail.process("ok\xC3"));
    assertEquals("", tail.flush());
    assertTrue(tail.empty());
    return EXIT_SUCCESS;
}

/**
 * Run all UTF-8 stream tests
 */
int test_utf8_stream() {
    RUN_TEST(test_utf8_stream_holds_partial);
    RUN_TEST(test_utf8_stream_flush);
    return EXIT_SUCCESS;
}
