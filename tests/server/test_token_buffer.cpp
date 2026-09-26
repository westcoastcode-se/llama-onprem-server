//
// File containing tests for the token stream buffer
//

#include "server/jobs/token_buffer.hpp"
#include "../tests.hpp"

/**
 * Pieces are pulled in order. Done is reported only after the buffer is empty.
 */
static int test_token_buffer_pull() {
    TokenBuffer buffer;
    buffer.push("a");
    buffer.push("b");
    bool done = true;
    const auto pieces = buffer.pull_available(done);
    assertEquals(2, static_cast<int>(pieces.size()));
    assertEquals("a", pieces[0]);
    assertEquals("b", pieces[1]);
    assertTrue(!done);

    buffer.set_done();
    const auto rest = buffer.pull_available(done);
    assertTrue(rest.empty());
    assertTrue(done);
    assertTrue(buffer.is_done());
    assertTrue(!buffer.wait_pull().has_value());
    return EXIT_SUCCESS;
}

/**
 * Cancel drops later pieces and ends the stream.
 */
static int test_token_buffer_cancel() {
    TokenBuffer buffer;
    buffer.push("a");
    buffer.cancel();
    buffer.push("b");
    assertTrue(buffer.is_cancelled());

    bool done = false;
    const auto pieces = buffer.pull_available(done);
    assertEquals(1, static_cast<int>(pieces.size()));
    assertEquals("a", pieces[0]);
    assertTrue(done);
    assertTrue(!buffer.wait_pull().has_value());
    return EXIT_SUCCESS;
}

/**
 * The full result is stored apart from the token pieces.
 */
static int test_token_buffer_full_result() {
    TokenBuffer buffer;
    buffer.set_full_result("hello");
    assertEquals("hello", buffer.full_result());
    return EXIT_SUCCESS;
}

/**
 * Run all token buffer tests
 */
int test_token_buffer() {
    RUN_TEST(test_token_buffer_pull);
    RUN_TEST(test_token_buffer_cancel);
    RUN_TEST(test_token_buffer_full_result);
    return EXIT_SUCCESS;
}
