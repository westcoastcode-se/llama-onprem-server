#include "llm/llm_engine.hpp"
#include "common/color.hpp"
#include "common/utf8.hpp"
#include "llama.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

LlamaEngine::LlamaEngine() = default;

LlamaEngine::~LlamaEngine() {
    free_resources();
}

void LlamaEngine::free_resources() {
    if (smpl_) {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
    chat_template_ = nullptr;
    cached_messages_.clear();
    formatted_buf_.clear();
    prev_formatted_len_ = 0;
}

bool LlamaEngine::init(const LlamaConfig & config, std::string & error) {
    free_resources();
    config_ = config;

    // only print critical llama errors
    llama_log_set([](enum ggml_log_level level, const char * text, void * /* user_data */) {
        if (level >= GGML_LOG_LEVEL_ERROR) {
            fprintf(stderr, "%s", text);
        }
    }, nullptr);

    // load dynamic backends
    ggml_backend_load_all();

    // initialize model
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config_.n_gpu_layers;

    printf("%s[agent] Loading model:%s %s\n", Color::CYAN, Color::RESET, config_.model_path.c_str());
    model_ = llama_model_load_from_file(config_.model_path.c_str(), model_params);
    if (!model_) {
        error = "unable to load model from " + config_.model_path;
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);
    chat_template_ = llama_model_chat_template(model_, nullptr);

    // initialize context
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config_.n_ctx;
    ctx_params.n_batch = std::max(1, std::min(config_.n_ctx, config_.n_batch));
    ctx_params.n_ubatch = std::min(ctx_params.n_batch, ctx_params.n_ubatch);

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
        error = "failed to create llama_context";
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }

    // initialize sampler
    smpl_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl_, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(smpl_, llama_sampler_init_temp(config_.temperature));
    llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    formatted_buf_.resize(llama_n_ctx(ctx_));
    prev_formatted_len_ = 0;

    return true;
}

void LlamaEngine::reset() {
    if (ctx_) {
        llama_memory_seq_rm(llama_get_memory(ctx_), 0, 0, -1);
    }
    cached_messages_.clear();
    prev_formatted_len_ = 0;
}

int LlamaEngine::get_context_size() const {
    return ctx_ ? llama_n_ctx(ctx_) : 0;
}

int LlamaEngine::get_used_context() const {
    return ctx_ ? (llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1) : 0;
}

int LlamaEngine::format_chat_internal(std::span<const Protocol::ChatMessage> msgs,
                                      bool add_assistant,
                                      std::vector<char> & out) const {
    if (!model_ || !chat_template_) return -1;

    std::vector<llama_chat_message> raw_msgs;
    raw_msgs.reserve(msgs.size());
    for (const auto & m : msgs) {
        raw_msgs.push_back({m.role.c_str(), m.content.c_str()});
    }

    if (out.size() < 4096) out.resize(4096);
    int len = llama_chat_apply_template(chat_template_, raw_msgs.data(), raw_msgs.size(), add_assistant, out.data(), out.size());
    if (len > (int)out.size()) {
        out.resize(len + 1);
        len = llama_chat_apply_template(chat_template_, raw_msgs.data(), raw_msgs.size(), add_assistant, out.data(), out.size());
    }
    return len;
}

std::string LlamaEngine::apply_template(std::span<const Protocol::ChatMessage> messages,
                                        bool add_assistant) const {
    std::vector<char> buf(4096);
    int len = format_chat_internal(messages, add_assistant, buf);
    if (len <= 0) return "";
    return std::string(buf.data(), len);
}

std::string LlamaEngine::generate(std::string_view prompt,
                                  TokenCallback token_cb,
                                  float temp_override) {
    if (!ctx_ || !vocab_ || !smpl_) return "";

    if (temp_override >= 0.0f && temp_override != config_.temperature) {
        llama_sampler_free(smpl_);
        smpl_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl_, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl_, llama_sampler_init_temp(temp_override));
        llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        config_.temperature = temp_override;
    }

    std::string response;
    const int max_ctx = llama_n_ctx(ctx_);

    auto context_would_overflow = [&](int n_new) -> bool {
        const int used_ctx = llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1;
        return used_ctx + n_new > max_ctx;
    };

    const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) == -1;

    // tokenize prompt
    const int n_prompt_tokens = -llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()), NULL, 0, is_first, true);
    if (n_prompt_tokens <= 0) {
        return "";
    }

    std::vector<llama_token> prompt_tokens(n_prompt_tokens);
    if (llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()), prompt_tokens.data(), prompt_tokens.size(), is_first, true) < 0) {
        fprintf(stderr, "error: failed to tokenize prompt\n");
        return "";
    }

    // evaluate prompt in chunks of n_batch
    const int batch_size = llama_n_batch(ctx_);
    for (size_t i = 0; i < prompt_tokens.size(); i += batch_size) {
        const int n_eval = std::min(static_cast<int>(prompt_tokens.size() - i), batch_size);
        llama_batch batch = llama_batch_get_one(prompt_tokens.data() + i, n_eval);

        if (context_would_overflow(batch.n_tokens)) {
            fprintf(stderr, "\n[Warning: Context size (%d) reached limit. Please reset context]\n", max_ctx);
            return "";
        }

        int ret = llama_decode(ctx_, batch);
        if (ret != 0) {
            fprintf(stderr, "error: failed to decode prompt batch (ret = %d)\n", ret);
            return "";
        }
    }

    // token generation loop
    Utf8Util::StreamBuffer utf8_buf;
    while (true) {
        llama_token new_token_id = llama_sampler_sample(smpl_, ctx_, -1);

        if (llama_vocab_is_eog(vocab_, new_token_id)) {
            break;
        }

        char buf[256];
        int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
        std::string_view piece;
        std::vector<char> big;
        if (n >= 0) {
            piece = std::string_view(buf, static_cast<size_t>(n));
        } else {
            big.resize(static_cast<size_t>(-n));
            n = llama_token_to_piece(vocab_, new_token_id, big.data(), static_cast<int>(big.size()), 0, true);
            if (n < 0) {
                break;
            }
            piece = std::string_view(big.data(), static_cast<size_t>(n));
        }

        response.append(piece);

        if (token_cb) {
            std::string ready = utf8_buf.process(piece);
            if (!ready.empty()) {
                if (!token_cb(ready)) {
                    // Consumer requested abort (e.g. client disconnected)
                    break;
                }
            }
        }

        if (context_would_overflow(1)) {
            fprintf(stderr, "\n[Warning: Context size (%d) reached limit. Please reset context]\n", max_ctx);
            break;
        }

        llama_batch batch = llama_batch_get_one(&new_token_id, 1);
        int ret = llama_decode(ctx_, batch);
        if (ret != 0) {
            fprintf(stderr, "error: failed to decode batch (ret = %d)\n", ret);
            break;
        }
    }

    if (token_cb) {
        std::string remaining = utf8_buf.flush();
        if (!remaining.empty()) {
            token_cb(remaining);
        }
    }

    return response;
}

std::string LlamaEngine::chat(std::span<const Protocol::ChatMessage> messages,
                              TokenCallback token_cb,
                              float temp_override) {
    if (!ctx_ || !vocab_ || !smpl_ || !chat_template_) return "";

    // Apply temperature override if given
    if (temp_override >= 0.0f && temp_override != config_.temperature) {
        llama_sampler_free(smpl_);
        smpl_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl_, llama_sampler_init_min_p(0.05f, 1));
        llama_sampler_chain_add(smpl_, llama_sampler_init_temp(temp_override));
        llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        config_.temperature = temp_override;
    }

    // Check if cached_messages_ is a prefix of messages
    bool is_prefix = true;
    if (cached_messages_.size() > messages.size()) {
        is_prefix = false;
    } else {
        for (size_t i = 0; i < cached_messages_.size(); ++i) {
            if (cached_messages_[i].role != messages[i].role ||
                cached_messages_[i].content != messages[i].content) {
                is_prefix = false;
                break;
            }
        }
    }

    if (!is_prefix || cached_messages_.empty()) {
        reset();
    }

    const int new_len = format_chat_internal(messages, true, formatted_buf_);
    if (new_len < 0) {
        fprintf(stderr, "error: failed to format chat template\n");
        return "";
    }

    std::string_view prompt(formatted_buf_.data() + prev_formatted_len_, static_cast<size_t>(new_len - prev_formatted_len_));

    std::string response = generate(prompt, token_cb);

    cached_messages_.assign(messages.begin(), messages.end());
    cached_messages_.push_back({"assistant", response});

    prev_formatted_len_ = format_chat_internal(cached_messages_, false, formatted_buf_);
    if (prev_formatted_len_ < 0) {
        prev_formatted_len_ = 0;
    }

    return response;
}
