#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/protocol.hpp"

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;
struct common_chat_templates;

struct LlamaConfig {
    std::string model_path;
    // Jinja file used instead of the template stored in the GGUF. Empty reads the model template.
    std::string template_path;
    // Template input enable_thinking.
    bool reasoning = true;
    int n_ctx = 4096;
    int n_batch = 2048;
    int n_gpu_layers = 99;
    int n_threads = 0;
    int n_threads_batch = 0;
    std::string flash_attn = "auto";
    std::string cache_type_k = "f16";
    std::string cache_type_v = "f16";
    float temperature = 0.7f;
    float frequency_penalty = 0.0f;
    int penalty_last_n = 64;
    uint32_t seed = 0xFFFFFFFFu;
    int max_tokens = -1;
};

// Return true to keep generating, false to abort. An abort rolls the KV cache back.
using TokenCallback = std::function<bool(std::string_view piece)>;

// Single-session engine used by the TCP server and the fat client.
// active_tokens_ is the token ids in the one KV sequence. Each call tokenizes the full prompt,
// keeps the matching prefix, and decodes only the tail. Failures restore that prefix.
// There is no per-session snapshot; the callisto server engine parks those separately.
class LlamaEngine {
public:
    LlamaEngine();
    ~LlamaEngine();

    LlamaEngine(const LlamaEngine &) = delete;
    LlamaEngine & operator=(const LlamaEngine &) = delete;

    bool init(const LlamaConfig & config, std::string & error);
    void reset();

    // max_tokens < 0 uses LlamaConfig::max_tokens. should_stop is polled between prompt batches.
    std::string generate(std::string_view prompt,
                         TokenCallback token_cb = nullptr,
                         float temp_override = -1.0f,
                         int max_tokens = -1,
                         std::function<bool()> should_stop = nullptr);

    std::string chat(std::span<const Protocol::ChatMessage> messages,
                     TokenCallback token_cb = nullptr,
                     float temp_override = -1.0f,
                     int max_tokens = -1,
                     std::function<bool()> should_stop = nullptr);

    std::string apply_template(std::span<const Protocol::ChatMessage> messages,
                               bool add_assistant = true) const;

    const LlamaConfig & get_config() const { return config_; }
    int get_context_size() const;
    int get_used_context() const;

private:
    void free_resources();
    void rebuild_sampler(float temperature);
    void trim_kv_to(size_t n_tokens);
    // Returns false when generation was aborted or decoding failed. On failure KV is restored to checkpoint.
    bool decode_tokens(std::span<const int32_t> tokens, const std::function<bool()> & should_stop, std::string & error);
    std::string format_messages(std::span<const Protocol::ChatMessage> messages, bool add_assistant, std::string & error) const;

    LlamaConfig config_;
    llama_model * model_ = nullptr;
    const llama_vocab * vocab_ = nullptr;
    llama_context * ctx_ = nullptr;
    llama_sampler * smpl_ = nullptr;
    // Parsed Jinja template. The C chat helper does not run Jinja.
    common_chat_templates * templates_ = nullptr;
    // Token ids currently in the KV sequence, in order. The next prompt is diffed against this.
    std::vector<int32_t> active_tokens_;
};
