#pragma once

#include "../api/messages.hpp"
#include "session_kv_store.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;
struct common_chat_templates;
class KvTrim;

// Process-wide model settings. Per-request overrides live on LlamaRequest.
struct LlamaConfig
{
    std::string model_path;
    // Jinja source used instead of the template embedded in the GGUF. Empty keeps the model template.
    std::string template_path;
    // Passed to the template as enable_thinking. Qwen opens a <think> block when this is true.
    bool reasoning = true;
    int n_ctx = 4096;
    int n_batch = 2048;
    int n_gpu_layers = 99;
    // 0 leaves llama.cpp's default thread counts.
    int n_threads = 0;
    int n_threads_batch = 0;
    // "auto", "on", or "off".
    std::string flash_attn = "auto";
    // KV cache dtypes (f16, f32, bf16, q8_0, ...). Smaller types free VRAM for a longer n_ctx.
    std::string cache_type_k = "f16";
    std::string cache_type_v = "f16";
    float temperature = 1.0f;
    float top_p = 0.95f;
    int top_k = 20;
    float min_p = 0.0f;
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;
    float repetition_penalty = 1.0f;
    int penalty_last_n = 64;
    // 0xFFFFFFFF is llama.cpp's random seed.
    uint32_t seed = 0xFFFFFFFFu;
    // New tokens per turn. Negative means "until the context is full".
    int max_tokens = -1;
    // Parsed so older command lines still start. Snapshots are files in session_dir and are not capped by count.
    int kv_sessions = 2;
    // 0 does not limit the session directory. Otherwise the oldest session files are removed until they fit.
    uint64_t session_cache_bytes = 0;
    // Parked session files: token ids, message ends, and the llama state bytes.
    std::string session_dir{kDefaultSessionDir};
};

struct ModelNotFound : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct LlamaModelInitError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct LlamaRuntimeError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The prompt does not fit, or the next token would. The turn is discarded, not stored as a finished reply.
struct LlamaContextFull : LlamaRuntimeError
{
    using LlamaRuntimeError::LlamaRuntimeError;
};

// Piece of generated text. Return true to keep going, false to abort the turn.
// move_only_function::operator() is non-const, so the members below are mutable:
// a const LlamaRequest can still invoke them.
using TokenCallback = std::move_only_function<bool(std::string &&piece)>;

// Controls for one generate() or chat() call. Does not change LlamaConfig.
struct LlamaRequest
{
    mutable TokenCallback token_cb{};
    // Polled between prompt batches, so a long prefill can be cancelled before the first token.
    mutable std::move_only_function<bool()> should_stop{};
    // Negative keeps LlamaConfig::temperature.
    float temp_override = -1.0f;
    // Negative keeps LlamaConfig::max_tokens.
    int max_tokens = -1;
    // KV-cache key. Empty is the anonymous slot. One context is shared, so sessions take turns in it.
    std::string session_id;
    // Called on the worker once the prompt is in the KV cache, before the first sampled token.
    mutable std::move_only_function<void()> on_prompt{};
    // Rendered into the Jinja template so the model sees the tool schema.
    std::vector<ChatTool> tools;
};

// One loaded GGUF and one llama_context.
//
// The context holds a single KV sequence. active_tokens_ is the token ids that sequence contains.
// Other sessions are parked with llama_state_seq_save_file: the sequence state and those ids.
// Switching sessions writes the live sequence out and reads the next one back, so a change is not
// a full prefill.
//
// A turn tokenizes its whole prompt and decodes only the tail that differs from active_tokens_.
// message_ends_ records where each rendered message ends in the live sequence. The parked file has
// no field for those ends, so the first turn after a load measures them again. While the session
// stays active the next turn reuses the longest end that still matches, then compares the generated
// tail token by token.
// Matching tokens, not formatted bytes, is what keeps end-of-turn markers in the cache: the
// model stops on EOG without decoding it, and the next prompt's template emits that marker again.
//
// Not thread-safe. Jobs calls chat, generate, and release_session from its worker thread.
class LlamaEngine
{
  public:
    LlamaEngine();
    ~LlamaEngine();

    LlamaEngine(const LlamaEngine &) = delete;
    LlamaEngine &operator=(const LlamaEngine &) = delete;
    LlamaEngine(LlamaEngine &&other) noexcept;
    LlamaEngine &operator=(LlamaEngine &&other) noexcept;

    // Loads the model. Throws on failure; the destructor frees whatever was allocated.
    static LlamaEngine create(const LlamaConfig &config);

    // Drops the live KV sequence. Parked sessions are left alone.
    void reset();

    // Deletes one session's parked file, and the live sequence when it is that session.
    // Call from the worker thread, never during llama_decode.
    void release_session(std::string_view session_id);

    // Copy one session's KV and token ids under a new id. The source session stays as it is.
    // Call from the worker thread, never during llama_decode.
    void clone_session(const std::string &from, const std::string &to);

    // Write the live sequence. The worker calls this when it stops, so a restart can load it.
    // Call from the worker thread, never during llama_decode.
    void park_active_session();

    // Tokenize prompt, reuse the matching KV prefix, then sample until EOG, max_tokens, or abort.
    // An aborted or failed turn rolls the cache back to the prefix it started from.
    [[nodiscard]] std::string generate(std::string_view prompt, const LlamaRequest &request = {});

    // Apply the chat template, then generate. tools and reasoning are template inputs, not sampler state.
    [[nodiscard]] std::string chat(std::span<const ChatMessage> messages, const LlamaRequest &request = {});

    // Template text only. Does not touch the KV cache.
    [[nodiscard]] std::string apply_template(std::span<const ChatMessage> messages, bool add_assistant = true,
                                             std::span<const ChatTool> tools = {}) const;

    [[nodiscard]] const LlamaConfig &get_config() const
    {
        return config_;
    }

    [[nodiscard]] int get_context_size() const;

    [[nodiscard]] int get_used_context() const;

    // Tokens stored for this session, live or parked. Worker thread only.
    [[nodiscard]] int session_token_count(std::string_view session_id) const;

    // Worker thread only. Empty when no session owns sequence 0.
    [[nodiscard]] const std::string &active_session_id() const
    {
        return active_session_id_;
    }

  private:
    struct ModelDeleter
    {
        void operator()(llama_model *model) const noexcept;
    };
    struct ContextDeleter
    {
        void operator()(llama_context *context) const noexcept;
    };
    struct SamplerDeleter
    {
        void operator()(llama_sampler *sampler) const noexcept;
    };
    struct TemplatesDeleter
    {
        void operator()(common_chat_templates *templates) const noexcept;
    };

    // New chain each call so a temperature override does not write back into config_.
    void rebuild_sampler(float temperature);
    // Make session_id the sequence in the context, parking the previous one if it differs.
    void activate_session(const std::string &session_id);
    // Load a parked file into sequence 0. False when this session has nothing saved.
    bool unpark_session(const std::string &session_id);
    // Keep the first n_tokens of the live sequence. The family's KvTrim decides whether a suffix
    // can be dropped. False clears the sequence, and the caller decodes its prompt from the start.
    [[nodiscard]] bool trim_kv_to(size_t n_tokens);
    // Decode tokens onto the live sequence. Appends each accepted batch to active_tokens_.
    void decode_tokens(std::span<const int32_t> tokens, std::move_only_function<bool()> &should_stop);

    // Jinja prompt. add_assistant leaves the template on the assistant turn so generation can continue it.
    [[nodiscard]] std::string format_messages(std::span<const ChatMessage> messages, bool add_assistant,
                                              std::span<const ChatTool> tools) const;

    LlamaConfig config_;
    std::unique_ptr<llama_model, ModelDeleter> model_;
    const llama_vocab *vocab_ = nullptr;
    std::unique_ptr<llama_context, ContextDeleter> ctx_;
    std::unique_ptr<llama_sampler, SamplerDeleter> smpl_;
    // Parsed Jinja. Owned here; llama.cpp's C chat API does not execute Jinja.
    std::unique_ptr<common_chat_templates, TemplatesDeleter> templates_;
    // How this model's cache drops a suffix. Chosen from the loaded architecture.
    std::unique_ptr<KvTrim> trim_;

    std::string active_session_id_;
    // Token ids currently stored in KV sequence 0, in order.
    std::vector<int32_t> active_tokens_;
    // End offset of each message inside active_tokens_. Empty until a chat turn has measured them.
    // A parked file does not store these, so the first turn after a load measures them again.
    std::vector<size_t> message_ends_;
    // Parked sessions live here as files. The live sequence stays in the context.
    SessionKvStore kv_store_;

    // End offset of each message inside prompt_tokens. Reuses message_ends_ while that prefix still matches.
    [[nodiscard]] std::vector<size_t> message_token_ends(std::span<const ChatMessage> messages,
                                                         std::span<const ChatTool> tools,
                                                         std::span<const int32_t> prompt_tokens) const;
    // prompt_tokens is the full prompt. message_ends are this turn's measured ends, empty for a raw prompt.
    [[nodiscard]] std::string generate_tokens(std::span<const int32_t> prompt_tokens, std::span<const size_t> message_ends,
                                              const LlamaRequest &request);
};
