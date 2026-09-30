#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct llama_context;
struct llama_model;

// How one model family drops the tail of the live KV sequence.
// False means the sequence was cleared and the caller decodes its prompt from the start.
class KvTrim
{
  public:
    virtual ~KvTrim() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;

    // n_tokens is already clamped to tokens.size(). tokens is resized to what the cache kept.
    virtual bool trim(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens,
                      std::string_view session_id) const = 0;
};

// Qwen3.8 keeps one recurrent state beside the attention cells. A suffix drop counts only when
// that state lands on the last kept token. Anything else clears the sequence.
class QwenKvTrim final : public KvTrim
{
  public:
    [[nodiscard]] std::string_view name() const override;
    bool trim(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens,
              std::string_view session_id) const override;
};

// An attention cache stores one cell per token. Removing the suffix leaves the tail on the last kept token.
class SuffixKvTrim final : public KvTrim
{
  public:
    [[nodiscard]] std::string_view name() const override;
    bool trim(llama_context *ctx, std::vector<int32_t> &tokens, size_t n_tokens,
              std::string_view session_id) const override;
};

// Hybrid and recurrent models use QwenKvTrim. Everyone else drops a suffix.
[[nodiscard]] std::unique_ptr<KvTrim> make_kv_trim(bool hybrid_or_recurrent);
[[nodiscard]] std::unique_ptr<KvTrim> make_kv_trim(const llama_model *model);
