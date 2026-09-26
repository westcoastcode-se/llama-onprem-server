#pragma once

#include "../api/messages.hpp"
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_vocab;

struct LlamaConfig
{
    std::string model_path;
    std::string template_path;
    bool resoning = true;
    int n_ctx = 4096;
    int n_batch = 2048;
    int n_gpu_layers = 99;
    float temperature = 1.0f;
    float top_p = 0.95f;
    int top_k = 20;
    float min_p = 0.0f;
    float presence_penalty = 0.0f;
    float repetition_penalty = 1.0f;
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

// Stream generated pieces while the LLM is running.
// Return true to continue generation, false to abort.
using TokenCallback = std::function<bool(std::string &&piece)>;

class LlamaEngine
{
  public:
    LlamaEngine() = default;
    ~LlamaEngine();

    LlamaEngine(const LlamaEngine &) = delete;
    LlamaEngine &operator=(const LlamaEngine &) = delete;
    LlamaEngine(LlamaEngine &&other) noexcept;
    LlamaEngine &operator=(LlamaEngine &&other) noexcept;

    static LlamaEngine create(const LlamaConfig &config);

    void reset();

    std::string generate(std::string_view prompt, TokenCallback token_cb = nullptr, float temp_override = -1.0f);

    /**
     * @param messages Messages to be sent to the LLM
     * @param token_cb Callback; return true to continue
     * @param temp_override Per-call temperature; does not mutate config
     * @return The complete assistant reply
     */
    std::string chat(std::span<const ChatMessage> messages, TokenCallback token_cb = nullptr,
                     float temp_override = -1.0f);

    [[nodiscard]] std::string apply_template(std::span<const ChatMessage> messages, bool add_assistant = true) const;

    [[nodiscard]] const LlamaConfig &get_config() const
    {
        return config_;
    }

    [[nodiscard]] int get_context_size() const;

    [[nodiscard]] int get_used_context() const;

  private:
    void destroy() noexcept;
    void rebuild_sampler(float temperature);
    int format_chat_internal(std::span<const ChatMessage> msgs, bool add_assistant, std::vector<char> &out) const;

    LlamaConfig config_;
    llama_model *model_ = nullptr;
    const llama_vocab *vocab_ = nullptr;
    llama_context *ctx_ = nullptr;
    llama_sampler *smpl_ = nullptr;
    const char *chat_template_ = nullptr;
    std::string chat_template_owned_;

    std::vector<ChatMessage> cached_messages_;
    std::vector<char> formatted_buf_;
    int prev_formatted_len_ = 0;
};
