//
// File containing tests for context parameter parsing
//

#include "server/llm/context_params.hpp"
#include "../tests.hpp"

/**
 * Known cache type names map to ggml types.
 */
static int test_parse_cache_type() {
    assertTrue(parse_cache_type("f32") == GGML_TYPE_F32);
    assertTrue(parse_cache_type("f16") == GGML_TYPE_F16);
    assertTrue(parse_cache_type("q8_0") == GGML_TYPE_Q8_0);

    bool threw = false;
    try
    {
        parse_cache_type("nope");
    }
    catch (const std::runtime_error &error)
    {
        threw = true;
        assertTrue(std::string(error.what()).find("unknown cache type") != std::string::npos);
    }
    assertTrue(threw);
    return EXIT_SUCCESS;
}

/**
 * Flash attention is on, off, or automatic.
 */
static int test_parse_flash_attn() {
    assertTrue(parse_flash_attn("auto") == LLAMA_FLASH_ATTN_TYPE_AUTO);
    assertTrue(parse_flash_attn("on") == LLAMA_FLASH_ATTN_TYPE_ENABLED);
    assertTrue(parse_flash_attn("off") == LLAMA_FLASH_ATTN_TYPE_DISABLED);

    bool threw = false;
    try
    {
        parse_flash_attn("nope");
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }
    assertTrue(threw);
    return EXIT_SUCCESS;
}

/**
 * Context size is at least one, and the batch cannot be larger than the context.
 */
static int test_apply_context_knobs() {
    llama_context_params ctx = llama_context_default_params();
    const auto threads = ctx.n_threads;
    const auto threads_batch = ctx.n_threads_batch;

    apply_context_knobs(ctx, 0, 99999, -1, 0, "on", "q8_0", "f16");
    assertEquals(static_cast<uint32_t>(1), ctx.n_ctx);
    assertEquals(static_cast<uint32_t>(1), ctx.n_batch);
    assertEquals(static_cast<uint32_t>(1), ctx.n_ubatch);
    assertEquals(threads, ctx.n_threads);
    assertEquals(threads_batch, ctx.n_threads_batch);
    assertTrue(ctx.flash_attn_type == LLAMA_FLASH_ATTN_TYPE_ENABLED);
    assertTrue(ctx.type_k == GGML_TYPE_Q8_0);
    assertTrue(ctx.type_v == GGML_TYPE_F16);
    assertTrue(!ctx.no_perf);

    apply_context_knobs(ctx, 128, 64, 4, 2, "off", "f32", "f32");
    assertEquals(static_cast<uint32_t>(128), ctx.n_ctx);
    assertEquals(static_cast<uint32_t>(64), ctx.n_batch);
    assertEquals(4, ctx.n_threads);
    assertEquals(2, ctx.n_threads_batch);
    assertTrue(ctx.flash_attn_type == LLAMA_FLASH_ATTN_TYPE_DISABLED);
    return EXIT_SUCCESS;
}

/**
 * Run all context parameter tests
 */
int test_context_params() {
    RUN_TEST(test_parse_cache_type);
    RUN_TEST(test_parse_flash_attn);
    RUN_TEST(test_apply_context_knobs);
    return EXIT_SUCCESS;
}
