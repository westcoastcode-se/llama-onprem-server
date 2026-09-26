#include "llm_engine.hpp"
#include "llama.h"
#include "utf8_stream.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

void LlamaEngine::destroy() noexcept
{
    if (smpl_)
    {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    if (ctx_)
    {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_)
    {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
    chat_template_ = nullptr;
    chat_template_owned_.clear();
    cached_messages_.clear();
    formatted_buf_.clear();
    prev_formatted_len_ = 0;
}

LlamaEngine::~LlamaEngine()
{
    destroy();
}

LlamaEngine::LlamaEngine(LlamaEngine &&other) noexcept
    : config_(std::move(other.config_)), model_(other.model_), vocab_(other.vocab_), ctx_(other.ctx_), smpl_(other.smpl_),
      chat_template_(other.chat_template_), chat_template_owned_(std::move(other.chat_template_owned_)),
      cached_messages_(std::move(other.cached_messages_)), formatted_buf_(std::move(other.formatted_buf_)),
      prev_formatted_len_(other.prev_formatted_len_)
{
    other.model_ = nullptr;
    other.vocab_ = nullptr;
    other.ctx_ = nullptr;
    other.smpl_ = nullptr;
    other.chat_template_ = nullptr;
    other.prev_formatted_len_ = 0;
    if (!chat_template_owned_.empty())
    {
        chat_template_ = chat_template_owned_.c_str();
    }
}

LlamaEngine &LlamaEngine::operator=(LlamaEngine &&other) noexcept
{
    if (this != &other)
    {
        destroy();
        config_ = std::move(other.config_);
        model_ = other.model_;
        vocab_ = other.vocab_;
        ctx_ = other.ctx_;
        smpl_ = other.smpl_;
        chat_template_owned_ = std::move(other.chat_template_owned_);
        chat_template_ = other.chat_template_;
        cached_messages_ = std::move(other.cached_messages_);
        formatted_buf_ = std::move(other.formatted_buf_);
        prev_formatted_len_ = other.prev_formatted_len_;
        other.model_ = nullptr;
        other.vocab_ = nullptr;
        other.ctx_ = nullptr;
        other.smpl_ = nullptr;
        other.chat_template_ = nullptr;
        other.prev_formatted_len_ = 0;
        if (!chat_template_owned_.empty())
        {
            chat_template_ = chat_template_owned_.c_str();
        }
    }
    return *this;
}

void LlamaEngine::rebuild_sampler(float temperature)
{
    if (smpl_)
    {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    smpl_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!smpl_)
    {
        throw LlamaModelInitError("failed to create sampler chain");
    }
    const bool penalties_on = config_.repetition_penalty != 1.0f || config_.presence_penalty != 0.0f;
    if (penalties_on && vocab_)
    {
        llama_sampler_chain_add(
            smpl_, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab_), 64, config_.repetition_penalty, 0.0f,
                                                config_.presence_penalty));
    }
    if (config_.top_k > 0)
    {
        llama_sampler_chain_add(smpl_, llama_sampler_init_top_k(config_.top_k));
    }
    if (config_.top_p < 1.0f)
    {
        llama_sampler_chain_add(smpl_, llama_sampler_init_top_p(config_.top_p, 1));
    }
    if (config_.min_p > 0.0f)
    {
        llama_sampler_chain_add(smpl_, llama_sampler_init_min_p(config_.min_p, 1));
    }
    llama_sampler_chain_add(smpl_, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
}

LlamaEngine LlamaEngine::create(const LlamaConfig &config)
{
    LlamaEngine engine;
    engine.config_ = config;

    llama_log_set(
        [](enum ggml_log_level level, const char *text, void *) {
            if (level >= GGML_LOG_LEVEL_ERROR)
            {
                fprintf(stderr, "%s", text);
            }
        },
        nullptr);

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config.n_gpu_layers;

    fprintf(stderr, "[llm] Loading model: %s\n", config.model_path.c_str());
    engine.model_ = llama_model_load_from_file(config.model_path.c_str(), model_params);
    if (!engine.model_)
    {
        throw ModelNotFound("unable to load model from " + config.model_path);
    }

    engine.vocab_ = llama_model_get_vocab(engine.model_);
    engine.chat_template_ = llama_model_chat_template(engine.model_, nullptr);
    if (!engine.chat_template_ && !config.template_path.empty())
    {
        std::ifstream in(config.template_path);
        if (!in)
        {
            llama_model_free(engine.model_);
            engine.model_ = nullptr;
            throw LlamaModelInitError("failed to read chat template from " + config.template_path);
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        engine.chat_template_owned_ = ss.str();
        engine.chat_template_ = engine.chat_template_owned_.c_str();
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config.n_ctx;
    ctx_params.n_batch = std::max(1, std::min(config.n_ctx, config.n_batch));
    ctx_params.n_ubatch = std::min(ctx_params.n_batch, ctx_params.n_ubatch);

    engine.ctx_ = llama_init_from_model(engine.model_, ctx_params);
    if (!engine.ctx_)
    {
        llama_model_free(engine.model_);
        engine.model_ = nullptr;
        throw LlamaModelInitError("failed to create llama_context for " + config.model_path);
    }

    try
    {
        engine.rebuild_sampler(config.temperature);
    }
    catch (...)
    {
        llama_free(engine.ctx_);
        engine.ctx_ = nullptr;
        llama_model_free(engine.model_);
        engine.model_ = nullptr;
        throw;
    }

    engine.formatted_buf_.resize(llama_n_ctx(engine.ctx_));
    engine.prev_formatted_len_ = 0;
    return engine;
}

void LlamaEngine::reset()
{
    if (ctx_)
    {
        llama_memory_seq_rm(llama_get_memory(ctx_), 0, 0, -1);
    }
    cached_messages_.clear();
    prev_formatted_len_ = 0;
}

int LlamaEngine::get_context_size() const
{
    return ctx_ ? static_cast<int>(llama_n_ctx(ctx_)) : 0;
}

int LlamaEngine::get_used_context() const
{
    return ctx_ ? (llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1) : 0;
}

int LlamaEngine::format_chat_internal(std::span<const ChatMessage> msgs, bool add_assistant,
                                      std::vector<char> &out) const
{
    if (!model_ || !chat_template_)
        return -1;

    std::vector<llama_chat_message> raw_msgs;
    raw_msgs.reserve(msgs.size());
    for (const auto &m : msgs)
    {
        raw_msgs.push_back({m.role.c_str(), m.content.c_str()});
    }

    if (out.size() < 4096)
        out.resize(4096);
    int len = llama_chat_apply_template(chat_template_, raw_msgs.data(), raw_msgs.size(), add_assistant, out.data(),
                                        static_cast<int32_t>(out.size()));
    if (len < 0)
    {
        return len;
    }
    if (len > static_cast<int>(out.size()))
    {
        out.resize(static_cast<size_t>(len) + 1);
        len = llama_chat_apply_template(chat_template_, raw_msgs.data(), raw_msgs.size(), add_assistant, out.data(),
                                        static_cast<int32_t>(out.size()));
    }
    return len;
}

std::string LlamaEngine::apply_template(std::span<const ChatMessage> messages, bool add_assistant) const
{
    std::vector<char> buf(4096);
    int len = format_chat_internal(messages, add_assistant, buf);
    if (len <= 0)
        return "";
    return std::string(buf.data(), static_cast<size_t>(len));
}

std::string LlamaEngine::generate(std::string_view prompt, TokenCallback token_cb, float temp_override)
{
    if (!ctx_ || !vocab_ || !smpl_)
        throw LlamaRuntimeError("LlamaEngine is not initialized");

    const float temperature = temp_override >= 0.0f ? temp_override : config_.temperature;
    rebuild_sampler(temperature);

    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_));

    auto context_would_overflow = [&](int n_new) -> bool {
        const int used_ctx = llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1;
        return used_ctx + n_new > max_ctx;
    };

    const bool is_first = llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) == -1;

    const int n_prompt_tokens =
        -llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, is_first, true);
    if (n_prompt_tokens <= 0)
    {
        throw LlamaRuntimeError("failed to tokenize prompt");
    }

    std::vector<llama_token> prompt_tokens(static_cast<size_t>(n_prompt_tokens));
    if (llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()), prompt_tokens.data(),
                       static_cast<int32_t>(prompt_tokens.size()), is_first, true) < 0)
    {
        throw LlamaRuntimeError("failed to tokenize prompt");
    }

    const int batch_size = static_cast<int>(llama_n_batch(ctx_));
    for (size_t i = 0; i < prompt_tokens.size(); i += static_cast<size_t>(batch_size))
    {
        const int n_eval = std::min(static_cast<int>(prompt_tokens.size() - i), batch_size);
        const llama_batch batch = llama_batch_get_one(prompt_tokens.data() + i, n_eval);

        if (context_would_overflow(batch.n_tokens))
        {
            throw LlamaRuntimeError("context size reached limit");
        }

        int ret = llama_decode(ctx_, batch);
        if (ret != 0)
        {
            throw LlamaRuntimeError("failed to decode prompt batch");
        }
    }

    std::string response;
    Utf8Util::StreamBuffer utf8_buf;
    while (true)
    {
        llama_token new_token_id = llama_sampler_sample(smpl_, ctx_, -1);

        if (llama_vocab_is_eog(vocab_, new_token_id))
        {
            break;
        }

        char buf[256];
        int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
        std::string_view piece;
        std::vector<char> big;
        if (n >= 0)
        {
            piece = std::string_view(buf, static_cast<size_t>(n));
        }
        else
        {
            big.resize(static_cast<size_t>(-n));
            n = llama_token_to_piece(vocab_, new_token_id, big.data(), static_cast<int>(big.size()), 0, true);
            if (n < 0)
            {
                break;
            }
            piece = std::string_view(big.data(), static_cast<size_t>(n));
        }

        response.append(piece);

        if (token_cb)
        {
            std::string ready = utf8_buf.process(piece);
            if (!ready.empty())
            {
                if (!token_cb(std::move(ready)))
                {
                    break;
                }
            }
        }

        if (context_would_overflow(1))
        {
            fprintf(stderr, "\n[Warning: Context size (%d) reached limit]\n", max_ctx);
            break;
        }

        llama_batch batch = llama_batch_get_one(&new_token_id, 1);
        int ret = llama_decode(ctx_, batch);
        if (ret != 0)
        {
            throw LlamaRuntimeError("failed to decode generation batch");
        }
    }

    if (token_cb)
    {
        std::string remaining = utf8_buf.flush();
        if (!remaining.empty())
        {
            token_cb(std::move(remaining));
        }
    }

    return response;
}

std::string LlamaEngine::chat(std::span<const ChatMessage> messages, TokenCallback token_cb, const float temp_override)
{
    if (!ctx_ || !vocab_ || !smpl_)
        throw LlamaRuntimeError("LlamaEngine is not initialized");
    if (!chat_template_)
        throw LlamaRuntimeError("no chat template in GGUF and no template_path configured");

    bool is_prefix = true;
    if (cached_messages_.size() > messages.size())
    {
        is_prefix = false;
    }
    else
    {
        for (size_t i = 0; i < cached_messages_.size(); ++i)
        {
            if (cached_messages_[i].role != messages[i].role || cached_messages_[i].content != messages[i].content)
            {
                is_prefix = false;
                break;
            }
        }
    }

    if (!is_prefix || cached_messages_.empty())
    {
        reset();
    }

    const int new_len = format_chat_internal(messages, true, formatted_buf_);
    if (new_len < 0)
    {
        throw LlamaRuntimeError("failed to format chat template");
    }

    std::string_view prompt(formatted_buf_.data() + prev_formatted_len_,
                            static_cast<size_t>(new_len - prev_formatted_len_));

    std::string response = generate(prompt, token_cb, temp_override);

    cached_messages_.assign(messages.begin(), messages.end());
    cached_messages_.push_back(ChatMessage{.role = ChatMessage::ROLE_ASSISTANT, .content = response});

    prev_formatted_len_ = format_chat_internal(cached_messages_, false, formatted_buf_);
    if (prev_formatted_len_ < 0)
    {
        prev_formatted_len_ = 0;
    }

    return response;
}
