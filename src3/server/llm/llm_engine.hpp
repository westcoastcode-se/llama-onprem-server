#pragma once

#include "../api/messages.hpp"
#include <functional>
#include <span>
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
    string template_path;
    bool resoning = true;
    int n_ctx = 4096;
    int n_batch = 2048;
    int n_gpu_layers = 99;
    float temperature = 0.7f;
};

struct ModelNotFound : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

struct LlamaModelInitError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Callback function in which we can use to stream token generation while the LLM is running.
// The supplied string is given to you from the LLM to do as you see fit.
//
// Return true if you want to abort the token generation
using TokenCallback = std::function<bool(string_view &&piece)>;

class LlamaEngine
{
  public:
    // How long token generation is allowed to run before forcefully stop it
    // TODO: Implement
    static constexpr std::chrono::seconds kFinishedTtl{300};

    static LlamaEngine create(const LlamaConfig &config);

    void destroy();

    void reset();

    string generate(std::string_view prompt, TokenCallback token_cb = nullptr, float temp_override = -1.0f);

    /**
     *
     * @param messages Messages to be sent to the LLM
     * @param token_cb Callback
     * @param temp_override
     * @return The complete chat after the LLM is done
     */
    string chat(span<const ChatMessage> messages, TokenCallback token_cb = nullptr,
                     float temp_override = -1.0f);

    [[nodiscard]] std::string apply_template(std::span<const ChatMessage> messages, bool add_assistant = true) const;

    [[nodiscard]] const LlamaConfig &get_config() const
    {
        return config_;
    }
    [[nodiscard]] int get_context_size() const;
    [[nodiscard]] int get_used_context() const;

  private:
    int format_chat_internal(std::span<const ChatMessage> msgs, bool add_assistant, std::vector<char> &out) const;

    LlamaConfig config_;
    llama_model *model_ = nullptr;
    const llama_vocab *vocab_ = nullptr;
    llama_context *ctx_ = nullptr;
    llama_sampler *smpl_ = nullptr;
    const char *chat_template_ = nullptr;

    std::vector<ChatMessage> cached_messages_;
    std::vector<char> formatted_buf_;
    int prev_formatted_len_ = 0;
};
