//
// File containing tests for defer and errdefer
//

#include "common/defer.hpp"
#include "../tests.hpp"

/**
 * defer runs when the scope ends.
 */
static int test_defer_runs_on_scope_exit() {
    int value = 0;
    {
        defer(value = 1);
        assertEquals(0, value);
    }
    assertEquals(1, value);
    return EXIT_SUCCESS;
}

/**
 * errdefer runs only when the scope is left by an exception.
 */
static int test_errdefer_runs_on_exception() {
    int value = 0;
    try
    {
        errdefer({ value = 1; });
        throw std::runtime_error("fail");
    }
    catch (const std::runtime_error &)
    {
    }
    assertEquals(1, value);

    int quiet = 0;
    {
        errdefer({ quiet = 1; });
    }
    assertEquals(0, quiet);
    return EXIT_SUCCESS;
}

/**
 * Run all defer tests
 */
int test_defer() {
    RUN_TEST(test_defer_runs_on_scope_exit);
    RUN_TEST(test_errdefer_runs_on_exception);
    return EXIT_SUCCESS;
}
