#include "kv_trim.hpp"

#include "common/log.hpp"
#include "kv_match.hpp"

#include "llama.h"

namespace
{

[[nodiscard]] bool tail_matches(llama_context *ctx, size_t n_tokens)
{
    return kv_tail_matches(n_tokens, llama_memory_seq_pos_max(llama_get_memory(ctx), 0));
}

void clear_sequence(llama_context *ctx, std::vector<int32_t> &tokens)
{
    llama_memory_t memory = llama_get_memory(ctx);
    if (!llama_memory_seq_rm(memory, 0, 0, -1))
    {
        llama_memory_clear(memory, true);
    }
    tokens.clear();
}

// Drop [n_tokens, end). False when the call refuses or the tail does not sit on the last kept token.
[[nodiscard]] bool drop_suffix(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens)
{
    llama_memory_t memory = llama_get_memory(ctx);
    if (!llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(n_tokens), -1))
    {
        return false;
    }
    tokens.resize(n_tokens);
    if (tail_matches(ctx, n_tokens))
    {
        return true;
    }
    return false;
}

void log_recompute(std::string_view kind, std::string_view session_id, size_t n_tokens)
{
    log_info("[llm] session ", session_id, " ", kind, " cannot drop a kv suffix at ", n_tokens,
             ", recomputing the prompt");
}

} // namespace

std::string_view QwenKvTrim::name() const
{
    return "qwen";
}

bool QwenKvTrim::trim(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens,
                      std::string_view session_id) const
{
    // The whole sequence is already the prefix. Nothing to drop.
    if (n_tokens == tokens.size() && tail_matches(ctx, n_tokens))
    {
        return true;
    }

    // seq_rm can report success while the recurrent tail stays behind the attention cells.
    // A kept prefix is real only when the tail sits on the last kept token.
    if (n_tokens < tokens.size() && drop_suffix(ctx, tokens, n_tokens))
    {
        return true;
    }

    log_recompute(name(), session_id, n_tokens);
    clear_sequence(ctx, tokens);
    return false;
}

std::string_view SuffixKvTrim::name() const
{
    return "suffix";
}

bool SuffixKvTrim::trim(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens,
                        std::string_view session_id) const
{
    if (n_tokens == tokens.size() && tail_matches(ctx, n_tokens))
    {
        return true;
    }

    if (n_tokens < tokens.size() && drop_suffix(ctx, tokens, n_tokens))
    {
        return true;
    }

    log_recompute(name(), session_id, n_tokens);
    clear_sequence(ctx, tokens);
    return false;
}

std::unique_ptr<KvTrim> make_kv_trim(bool hybrid_or_recurrent)
{
    if (hybrid_or_recurrent)
    {
        return std::make_unique<QwenKvTrim>();
    }
    return std::make_unique<SuffixKvTrim>();
}

std::unique_ptr<KvTrim> make_kv_trim(const llama_model *model)
{
    const bool hybrid = model != nullptr && (llama_model_is_hybrid(model) || llama_model_is_recurrent(model));
    return make_kv_trim(hybrid);
}
