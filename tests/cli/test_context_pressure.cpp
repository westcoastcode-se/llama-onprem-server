#include "cli/agent/context_pressure.hpp"
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

int test_auto_compact_is_only_for_a_waiting_subagent()
{
    assertTrue(should_auto_compact(900, 1000, true, true));
    assertTrue(!should_auto_compact(800, 1000, true, true));
    assertTrue(!should_auto_compact(801, 1000, false, true));
    assertTrue(!should_auto_compact(900, 1000, true, false));
    assertTrue(!should_auto_compact(900, 0, true, true));
    return EXIT_SUCCESS;
}

int test_truncate_transcript_keeps_the_ends()
{
    assertEquals(std::string("short"), truncate_transcript("short", 100));

    constexpr std::string_view kGap = "\n...[earlier conversation omitted]...\n";
    constexpr int budget = 100;
    const std::size_t head = static_cast<std::size_t>(budget) / 5;
    const std::size_t tail = static_cast<std::size_t>(budget) - head - kGap.size();
    const std::string text = std::string(50, 'H') + std::string(100, 'M') + std::string(50, 'T');
    const std::string cut = truncate_transcript(text, budget);
    assertEquals(std::string(head, 'H') + std::string(kGap) + std::string(tail, 'T'), cut);

    assertEquals(std::string(8, 'Z'), truncate_transcript(std::string(30, 'Z'), 8));

    const std::string unknown = truncate_transcript(std::string(9000, 'Q'), 0);
    assertTrue(unknown.size() < 9000);
    assertTrue(unknown.find("earlier conversation omitted") != std::string::npos);
    return EXIT_SUCCESS;
}

} // namespace

int test_context_pressure()
{
    RUN_TEST(test_context_above_is_strict);
    RUN_TEST(test_context_percent_matches_the_meter);
    RUN_TEST(test_compact_offer_needs_a_waiting_interactive_client);
    RUN_TEST(test_auto_compact_is_only_for_a_waiting_subagent);
    RUN_TEST(test_truncate_transcript_keeps_the_ends);
    return EXIT_SUCCESS;
}
