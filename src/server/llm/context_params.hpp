#pragma once

#include "llama.h"

#include <algorithm>
#include <stdexcept>
#include <string>

inline ggml_type parse_cache_type(const std::string &name)
{
    if (name == "f32")
        return GGML_TYPE_F32;
    if (name == "f16")
        return GGML_TYPE_F16;
    if (name == "bf16")
        return GGML_TYPE_BF16;
    if (name == "q8_0")
        return GGML_TYPE_Q8_0;
    if (name == "q4_0")
        return GGML_TYPE_Q4_0;
    if (name == "q4_1")
        return GGML_TYPE_Q4_1;
    if (name == "q5_0")
        return GGML_TYPE_Q5_0;
    if (name == "q5_1")
        return GGML_TYPE_Q5_1;
    if (name == "iq4_nl")
        return GGML_TYPE_IQ4_NL;
    throw std::runtime_error("unknown cache type: " + name);
}

inline llama_flash_attn_type parse_flash_attn(const std::string &name)
{
    if (name == "auto")
        return LLAMA_FLASH_ATTN_TYPE_AUTO;
    if (name == "on")
        return LLAMA_FLASH_ATTN_TYPE_ENABLED;
    if (name == "off")
        return LLAMA_FLASH_ATTN_TYPE_DISABLED;
    throw std::runtime_error("unknown flash attention mode: " + name);
}

inline void apply_context_knobs(llama_context_params &ctx, int n_ctx, int n_batch, int n_threads, int n_threads_batch,
                                const std::string &flash, const std::string &type_k, const std::string &type_v)
{
    ctx.n_ctx = static_cast<uint32_t>(std::max(1, n_ctx));
    ctx.n_batch = static_cast<uint32_t>(std::max(1, std::min(n_ctx, n_batch)));
    ctx.n_ubatch = std::min(ctx.n_batch, ctx.n_ubatch);
    if (n_threads > 0)
    {
        ctx.n_threads = n_threads;
    }
    if (n_threads_batch > 0)
    {
        ctx.n_threads_batch = n_threads_batch;
    }
    ctx.flash_attn_type = parse_flash_attn(flash);
    ctx.type_k = parse_cache_type(type_k);
    ctx.type_v = parse_cache_type(type_v);
    // llama.cpp leaves perf counters off unless this is cleared.
    ctx.no_perf = false;
}
