#include "cli/context_pressure.hpp"
#include "../tests.hpp"

namespace
{

int test_context_above_is_strict()
{
    assertTrue(!context_above(800, 1000, 80));
    assertTrue(context_above(801, 1000, 80));
    assertTrue(context_above(1000, 1000, 80));
    assertTrue(!context_above(0, 1000, 80));
    assertTrue(!context_above(-1, 1000, 80));
    assertTrue(!context_above(900, 0, 80));
    assertTrue(!context_above(900, -1, 80));
    assertTrue(!context_above(900, 1000, 0));
    return EXIT_SUCCESS;
}

int test_context_percent_matches_the_meter()
{
    assertEquals(0, context_percent(10, 0));
    assertEquals(80, context_percent(800, 1000));
    assertEquals(80, context_percent(801, 1000));
    assertEquals(81, context_percent(805, 1000));
    assertEquals(0, context_percent(-5, 1000));
    assertEquals(999, context_percent(100000, 10));
    return EXIT_SUCCESS;
}

int test_compact_offer_needs_a_waiting_interactive_client()
{
    assertTrue(should_offer_compact(900, 1000, true, true));
    assertTrue(!should_offer_compact(800, 1000, true, true));
    assertTrue(!should_offer_compact(900, 1000, false, true));
    assertTrue(!should_offer_compact(900, 1000, true, false));
    assertTrue(!should_offer_compact(900, 0, true, true));
    return EXIT_SUCCESS;
}

} // namespace

int test_context_pressure()
{
    RUN_TEST(test_context_above_is_strict);
    RUN_TEST(test_context_percent_matches_the_meter);
    RUN_TEST(test_compact_offer_needs_a_waiting_interactive_client);
    return EXIT_SUCCESS;
}
