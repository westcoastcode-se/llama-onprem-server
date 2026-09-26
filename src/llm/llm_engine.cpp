#include "llm/llm_engine.hpp"

#include "common/color.hpp"
#include "common/span_prefix.hpp"
#include "common/utf8.hpp"
#include "llm/context_params.hpp"

#include "chat.h"
#include "llama.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

struct Abort {};

std::string read_text_file(const std::string & path, std::string & error) {
    std::ifstream in(path);
    if (!in) {
        error = "failed to read chat template from " + path;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<int32_t> tokenize_prompt(const llama_vocab * vocab, std::string_view text, std::string & error) {
    const int32_t probe = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, true, true);
    if (probe == 0) {
        error = "failed to tokenize prompt";
        return {};
    }
    const int32_t need = probe < 0 ? -probe : probe;
    std::vector<int32_t> out(static_cast<size_t>(need));
    const int32_t got = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), out.data(), need, true, true);
    if (got < 0) {
        error = "failed to tokenize prompt";
        return {};
    }
    out.resize(static_cast<size_t>(got));
    return out;
}

void log_perf(llama_context * ctx) {
    const auto perf = llama_perf_context(ctx);
    const double prefill = perf.t_p_eval_ms > 0.0 ? (1000.0 * perf.n_p_eval / perf.t_p_eval_ms) : 0.0;
    const double decode = perf.t_eval_ms > 0.0 ? (1000.0 * perf.n_eval / perf.t_eval_ms) : 0.0;
    fprintf(stderr, "[llm] prefill %d tok in %.1f ms (%.1f tok/s); decode %d tok in %.1f ms (%.1f tok/s)\n",
            perf.n_p_eval, perf.t_p_eval_ms, prefill, perf.n_eval, perf.t_eval_ms, decode);
}

} // namespace

LlamaEngine::LlamaEngine() = default;

LlamaEngine::~LlamaEngine() {
    free_resources();
}

void LlamaEngine::free_resources() {
    if (smpl_) {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    if (templates_) {
        common_chat_templates_free(templates_);
        templates_ = nullptr;
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
    active_tokens_.clear();
}

void LlamaEngine::rebuild_sampler(float temperature) {
    llama_sampler * chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain) {
        throw std::runtime_error("failed to create sampler chain");
    }
    if (config_.frequency_penalty != 0.0f && vocab_) {
        llama_sampler_chain_add(chain, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), config_.penalty_last_n, 1.0f, config_.frequency_penalty, 0.0f));
    }
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(0.05f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(config_.seed));
    if (smpl_) {
        llama_sampler_free(smpl_);
    }
    smpl_ = chain;
}

bool LlamaEngine::init(const LlamaConfig & config, std::string & error) {
    free_resources();
    config_ = config;

    llama_log_set([](enum ggml_log_level level, const char * text, void *) {
        if (level >= GGML_LOG_LEVEL_ERROR) {
            fprintf(stderr, "%s", text);
        }
    }, nullptr);

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config_.n_gpu_layers;

    printf("%s[agent] Loading model:%s %s\n", Color::CYAN, Color::RESET, config_.model_path.c_str());
    model_ = llama_model_load_from_file(config_.model_path.c_str(), model_params);
    if (!model_) {
        error = "unable to load model from " + config_.model_path;
        free_resources();
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);

    try {
        std::string override_src;
        if (!config_.template_path.empty()) {
            override_src = read_text_file(config_.template_path, error);
            if (override_src.empty()) {
                free_resources();
                return false;
            }
        }
        templates_ = common_chat_templates_init(model_, override_src).release();

        llama_context_params ctx_params = llama_context_default_params();
        apply_context_knobs(ctx_params, config_.n_ctx, config_.n_batch, config_.n_threads, config_.n_threads_batch,
                            config_.flash_attn, config_.cache_type_k, config_.cache_type_v);
        ctx_ = llama_init_from_model(model_, ctx_params);
        if (!ctx_) {
            error = "failed to create llama_context";
            free_resources();
            return false;
        }
        rebuild_sampler(config_.temperature);
    } catch (const std::exception & e) {
        error = e.what();
        free_resources();
        return false;
    }
    return true;
}

void LlamaEngine::reset() {
    trim_kv_to(0);
}

void LlamaEngine::trim_kv_to(size_t n_tokens) {
    if (n_tokens > active_tokens_.size()) {
        n_tokens = active_tokens_.size();
    }
    if (ctx_) {
        llama_memory_seq_rm(llama_get_memory(ctx_), 0, static_cast<llama_pos>(n_tokens), -1);
    }
    active_tokens_.resize(n_tokens);
}

int LlamaEngine::get_context_size() const {
    return ctx_ ? static_cast<int>(llama_n_ctx(ctx_)) : 0;
}

int LlamaEngine::get_used_context() const {
    return ctx_ ? (llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1) : 0;
}

std::string LlamaEngine::format_messages(std::span<const Protocol::ChatMessage> messages, bool add_assistant,
                                         std::string & error) const {
    if (!templates_) {
        error = "no chat template";
        return {};
    }
    try {
        common_chat_templates_inputs inputs;
        inputs.use_jinja = true;
        inputs.add_generation_prompt = add_assistant;
        inputs.enable_thinking = config_.reasoning;
        inputs.messages.reserve(messages.size());
        for (const auto & message : messages) {
            common_chat_msg msg;
            msg.role = message.role;
            msg.content = message.content;
            inputs.messages.push_back(std::move(msg));
        }
        try {
            return common_chat_templates_apply(templates_, inputs).prompt;
        } catch (const std::exception &) {
            inputs.force_pure_content = true;
            return common_chat_templates_apply(templates_, inputs).prompt;
        }
    } catch (const std::exception & e) {
        error = std::string("failed to format chat template: ") + e.what();
        return {};
    }
}

std::string LlamaEngine::apply_template(std::span<const Protocol::ChatMessage> messages, bool add_assistant) const {
    std::string error;
    std::string text = format_messages(messages, add_assistant, error);
    if (!error.empty()) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return {};
    }
    return text;
}

bool LlamaEngine::decode_tokens(std::span<const int32_t> tokens, const std::function<bool()> & should_stop, std::string & error) {
    if (tokens.empty()) {
        return true;
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_));
    const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_)));
    for (size_t offset = 0; offset < tokens.size();) {
        if (should_stop && should_stop()) {
            throw Abort{};
        }
        const int n_eval = std::min(batch_size, static_cast<int>(tokens.size() - offset));
        if (static_cast<int>(active_tokens_.size()) + n_eval > max_ctx) {
            error = "context size reached limit";
            return false;
        }
        llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(tokens.data() + offset), n_eval);
        const int ret = llama_decode(ctx_, batch);
        if (ret != 0) {
            error = "failed to decode prompt batch";
            return false;
        }
        active_tokens_.insert(active_tokens_.end(), tokens.begin() + static_cast<std::ptrdiff_t>(offset),
                              tokens.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<size_t>(n_eval)));
        offset += static_cast<size_t>(n_eval);
    }
    return true;
}

std::string LlamaEngine::generate(std::string_view prompt, TokenCallback token_cb, float temp_override, int max_tokens,
                                  std::function<bool()> should_stop) {
    if (!ctx_ || !vocab_ || !smpl_) {
        return {};
    }

    const float temperature = temp_override >= 0.0f ? temp_override : config_.temperature;
    rebuild_sampler(temperature);
    llama_perf_context_reset(ctx_);
    struct PerfGuard {
        llama_context * ctx = nullptr;
        ~PerfGuard() {
            if (ctx) {
                log_perf(ctx);
            }
        }
    } perf{ctx_};

    std::string error;
    const std::vector<int32_t> prompt_tokens = tokenize_prompt(vocab_, prompt, error);
    if (prompt_tokens.empty()) {
        fprintf(stderr, "error: %s\n", error.empty() ? "failed to tokenize prompt" : error.c_str());
        return {};
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(prompt_tokens.size()) >= max_ctx) {
        fprintf(stderr, "\n[Warning: Context size (%d) reached limit]\n", max_ctx);
        return {};
    }

    // Shared token prefix stays in the cache. The tail is decoded again, which is where an
    // end-of-turn marker emitted by the template shows up on the next call.
    const size_t checkpoint = common_prefix_length(std::span<const int32_t>(active_tokens_),
                                                   std::span<const int32_t>(prompt_tokens));
    trim_kv_to(checkpoint);
    // Abort and decode errors must not leave a partial turn in the cache.
    auto rollback = [&] { trim_kv_to(checkpoint); };

    std::string response;
    try {
        if (!decode_tokens(std::span<const int32_t>(prompt_tokens).subspan(checkpoint), should_stop, error)) {
            fprintf(stderr, "\n[Warning: %s]\n", error.c_str());
            rollback();
            return {};
        }

        Utf8Util::StreamBuffer utf8_buf;
        const int max_new = max_tokens >= 0 ? max_tokens : config_.max_tokens;
        int generated = 0;
        while (max_new < 0 || generated < max_new) {
            if (should_stop && should_stop()) {
                throw Abort{};
            }
            if (static_cast<int>(active_tokens_.size()) >= max_ctx) {
                fprintf(stderr, "\n[Warning: Context size (%d) reached limit]\n", max_ctx);
                rollback();
                return response;
            }

            const llama_token new_token_id = llama_sampler_sample(smpl_, ctx_, -1);
            // Leave EOG out of the cache. The next formatted prompt contains the end marker as text.
            if (llama_vocab_is_eog(vocab_, new_token_id)) {
                break;
            }

            char buf[256];
            int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
            std::string piece;
            if (n >= 0) {
                piece.assign(buf, static_cast<size_t>(n));
            } else {
                piece.resize(static_cast<size_t>(-n));
                n = llama_token_to_piece(vocab_, new_token_id, piece.data(), static_cast<int>(piece.size()), 0, true);
                if (n < 0) {
                    fprintf(stderr, "error: failed to detokenize\n");
                    rollback();
                    return {};
                }
                piece.resize(static_cast<size_t>(n));
            }
            response.append(piece);

            if (token_cb) {
                std::string ready = utf8_buf.process(piece);
                if (!ready.empty() && !token_cb(ready)) {
                    throw Abort{};
                }
            }

            llama_token id = new_token_id;
            llama_batch batch = llama_batch_get_one(&id, 1);
            if (llama_decode(ctx_, batch) != 0) {
                fprintf(stderr, "error: failed to decode generation batch\n");
                rollback();
                return {};
            }
            active_tokens_.push_back(new_token_id);
            ++generated;
        }

        if (token_cb) {
            std::string remaining = utf8_buf.flush();
            if (!remaining.empty()) {
                token_cb(remaining);
            }
        }
        return response;
    } catch (const Abort &) {
        rollback();
        return response;
    } catch (const std::exception & e) {
        fprintf(stderr, "error: %s\n", e.what());
        rollback();
        return {};
    }
}

std::string LlamaEngine::chat(std::span<const Protocol::ChatMessage> messages, TokenCallback token_cb, float temp_override,
                              int max_tokens, std::function<bool()> should_stop) {
    if (!ctx_ || !vocab_ || !smpl_ || !templates_) {
        return {};
    }
    std::string error;
    const std::string prompt = format_messages(messages, true, error);
    if (!error.empty()) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return {};
    }
    return generate(prompt, std::move(token_cb), temp_override, max_tokens, std::move(should_stop));
}
