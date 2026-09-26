//
// File containing tests for web helper functions
//

#include "common/tools/web_utils.hpp"
#include "../tests.hpp"

/**
 * Spaces and reserved characters are percent-encoded. Plain text is left alone.
 */
static int test_url_encode() {
    assertEquals("hello", Tools::WebUtils::url_encode("hello"));
    assertEquals("a%20b", Tools::WebUtils::url_encode("a b"));
    assertEquals("a%2Fb", Tools::WebUtils::url_encode("a/b"));
    assertEquals("", Tools::WebUtils::url_encode(""));
    return EXIT_SUCCESS;
}

/**
 * Tags and entities become plain text.
 */
static int test_html_to_text_tags_and_entities() {
    const auto text = Tools::WebUtils::html_to_text("<p>Hello &amp; world</p>");
    assertEquals("Hello & world ", text);
    return EXIT_SUCCESS;
}

/**
 * Script and style blocks are removed, not shown as text.
 */
static int test_html_to_text_strips_script() {
    const auto text = Tools::WebUtils::html_to_text("<script>bad()</script><style>.x{}</style>Hi");
    assertEquals("Hi", text);
    return EXIT_SUCCESS;
}

/**
 * Adjacent blocks are separated by a space.
 */
static int test_html_to_text_blocks() {
    const auto text = Tools::WebUtils::html_to_text("<div>A</div><div>B</div>");
    assertEquals("A B ", text);
    return EXIT_SUCCESS;
}

/**
 * Run all web helper tests
 */
int test_web_utils() {
    RUN_TEST(test_url_encode);
    RUN_TEST(test_html_to_text_tags_and_entities);
    RUN_TEST(test_html_to_text_strips_script);
    RUN_TEST(test_html_to_text_blocks);
    return EXIT_SUCCESS;
}
