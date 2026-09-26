#include "llm_engine.hpp"

#include "common/log.hpp"
#include "common/span_prefix.hpp"
#include "llm/context_params.hpp"
#include "utf8_stream.hpp"

#include "chat.h"
#include "llama.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace
{

// Internal only. generate() catches this, rolls the cache back, and returns the text produced so far.
// It must not escape: Jobs treats other exceptions as a failed turn.
struct LlamaAbort : std::exception
{
    const char *what() const noexcept override
    {
        return "generation aborted";
    }
};

std::string read_text_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw LlamaModelInitError("failed to read chat template from " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int effective_max_tokens(const LlamaConfig &config, const LlamaRequest &request)
{
    return request.max_tokens >= 0 ? request.max_tokens : config.max_tokens;
}

// add_special and parse_special stay true for every full prompt. The cached prefix was tokenized
// the same way, so a BOS token or a special token written by the template does not shift the diff.
std::vector<int32_t> tokenize_prompt(const llama_vocab *vocab, std::string_view text)
{
    const int32_t probe = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, true, true);
    if (probe == 0)
    {
        return {};
    }
    const int32_t need = probe < 0 ? -probe : probe;
    std::vector<int32_t> out(static_cast<size_t>(need));
    const int32_t got = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), out.data(), need, true, true);
    if (got < 0)
    {
        throw LlamaRuntimeError("failed to tokenize prompt");
    }
    out.resize(static_cast<size_t>(got));
    return out;
}

void log_perf(llama_context *ctx)
{
    const auto perf = llama_perf_context(ctx);
    const double prefill = perf.t_p_eval_ms > 0.0 ? (1000.0 * perf.n_p_eval / perf.t_p_eval_ms) : 0.0;
    const double decode = perf.t_eval_ms > 0.0 ? (1000.0 * perf.n_eval / perf.t_eval_ms) : 0.0;
    log_info("[llm] prefill ", perf.n_p_eval, " tok in ", perf.t_p_eval_ms, " ms (", prefill,
             " tok/s); decode ", perf.n_eval, " tok in ", perf.t_eval_ms, " ms (", decode, " tok/s)");
}

} // namespace

void LlamaEngine::destroy() noexcept
{
    if (smpl_)
    {
        llama_sampler_free(smpl_);
        smpl_ = nullptr;
    }
    if (templates_)
    {
        common_chat_templates_free(templates_);
        templates_ = nullptr;
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
    active_session_id_.clear();
    active_tokens_.clear();
    stored_sessions_.clear();
}

LlamaEngine::~LlamaEngine()
{
    destroy();
}

LlamaEngine::LlamaEngine(LlamaEngine &&other) noexcept
    : config_(std::move(other.config_)), model_(other.model_), vocab_(other.vocab_), ctx_(other.ctx_), smpl_(other.smpl_),
      templates_(other.templates_), active_session_id_(std::move(other.active_session_id_)),
      active_tokens_(std::move(other.active_tokens_)), stored_sessions_(std::move(other.stored_sessions_))
{
    other.model_ = nullptr;
    other.vocab_ = nullptr;
    other.ctx_ = nullptr;
    other.smpl_ = nullptr;
    other.templates_ = nullptr;
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
        templates_ = other.templates_;
        active_session_id_ = std::move(other.active_session_id_);
        active_tokens_ = std::move(other.active_tokens_);
        stored_sessions_ = std::move(other.stored_sessions_);
        other.model_ = nullptr;
        other.vocab_ = nullptr;
        other.ctx_ = nullptr;
        other.smpl_ = nullptr;
        other.templates_ = nullptr;
    }
    return *this;
}

void LlamaEngine::rebuild_sampler(float temperature)
{
    // Built into a local chain first so a failure leaves the previous sampler in place.
    llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain)
    {
        throw LlamaModelInitError("failed to create sampler chain");
    }
    try
    {
        const bool penalties_on = config_.repetition_penalty != 1.0f || config_.presence_penalty != 0.0f ||
                                  config_.frequency_penalty != 0.0f;
        if (penalties_on && vocab_)
        {
            llama_sampler_chain_add(chain, llama_sampler_init_penalties(
                                               llama_vocab_n_tokens(vocab_), config_.penalty_last_n, config_.repetition_penalty,
                                               config_.frequency_penalty, config_.presence_penalty));
        }
        if (config_.top_k > 0)
        {
            llama_sampler_chain_add(chain, llama_sampler_init_top_k(config_.top_k));
        }
        if (config_.top_p < 1.0f)
        {
            llama_sampler_chain_add(chain, llama_sampler_init_top_p(config_.top_p, 1));
        }
        if (config_.min_p > 0.0f)
        {
            llama_sampler_chain_add(chain, llama_sampler_init_min_p(config_.min_p, 1));
        }
        llama_sampler_chain_add(chain, llama_sampler_init_temp(temperature));
        llama_sampler_chain_add(chain, llama_sampler_init_dist(config_.seed));
    }
    catch (...)
    {
        llama_sampler_free(chain);
        throw;
    }
    if (smpl_)
    {
        llama_sampler_free(smpl_);
    }
    smpl_ = chain;
}

LlamaEngine LlamaEngine::create(const LlamaConfig &config)
{
    LlamaEngine engine;
    engine.config_ = config;
    if (engine.config_.kv_sessions < 1)
    {
        engine.config_.kv_sessions = 1;
    }
    if (engine.config_.n_ctx <= 0)
    {
        throw LlamaModelInitError("n_ctx must be positive");
    }

    llama_log_set(
        [](enum ggml_log_level level, const char *text, void *) {
            if (level >= GGML_LOG_LEVEL_ERROR)
            {
                log_error(text);
            }
        },
        nullptr);

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config.n_gpu_layers;

    log_info("[llm] loading model ", config.model_path);
    engine.model_ = llama_model_load_from_file(config.model_path.c_str(), model_params);
    if (!engine.model_)
    {
        throw ModelNotFound("unable to load model from " + config.model_path);
    }

    engine.vocab_ = llama_model_get_vocab(engine.model_);

    std::string override_src;
    if (!config.template_path.empty())
    {
        override_src = read_text_file(config.template_path);
        if (override_src.empty())
        {
            throw LlamaModelInitError("chat template is empty: " + config.template_path);
        }
        log_info("[llm] chat template ", config.template_path);
    }
    else
    {
        log_info("[llm] chat template from model");
    }
    // A non-empty override replaces the GGUF template. An empty string reads the template from the model.
    engine.templates_ = common_chat_templates_init(engine.model_, override_src).release();

    llama_context_params ctx_params = llama_context_default_params();
    apply_context_knobs(ctx_params, config.n_ctx, config.n_batch, config.n_threads, config.n_threads_batch,
                        config.flash_attn, config.cache_type_k, config.cache_type_v);

    engine.ctx_ = llama_init_from_model(engine.model_, ctx_params);
    if (!engine.ctx_)
    {
        throw LlamaModelInitError("failed to create llama_context for " + config.model_path);
    }

    engine.rebuild_sampler(config.temperature);
    return engine;
}

void LlamaEngine::reset()
{
    trim_kv_to(0);
}

void LlamaEngine::erase_stored(const std::string &session_id)
{
    std::erase_if(stored_sessions_, [&](const SessionKv &slot) { return slot.id == session_id; });
}

void LlamaEngine::release_session(std::string_view session_id)
{
    const std::string id(session_id);
    erase_stored(id);
    if (active_session_id_ == id)
    {
        trim_kv_to(0);
    }
}

void LlamaEngine::trim_kv_to(size_t n_tokens)
{
    // Positions and active_tokens_ describe the same sequence. Trimming one without the other
    // makes the next prefix diff decode tokens on top of the wrong cache.
    if (n_tokens > active_tokens_.size())
    {
        n_tokens = active_tokens_.size();
    }
    if (ctx_)
    {
        llama_memory_seq_rm(llama_get_memory(ctx_), 0, static_cast<llama_pos>(n_tokens), -1);
    }
    active_tokens_.resize(n_tokens);
}

void LlamaEngine::park_active_session()
{
    // The context can hold one sequence. Parking copies it out so another session can use sequence 0.
    // kv_sessions counts the live sequence too, so at most kv_sessions-1 snapshots stay in memory.
    erase_stored(active_session_id_);
    const int keep = std::max(0, config_.kv_sessions - 1);
    if (keep > 0 && ctx_ && !active_tokens_.empty())
    {
        const size_t bytes = llama_state_seq_get_size(ctx_, 0);
        if (bytes == 0)
        {
            log_error("[llm] session ", active_session_id_, " kv save returned empty state");
        }
        else
        {
            SessionKv slot;
            slot.id = active_session_id_;
            slot.tokens = active_tokens_;
            slot.used = std::chrono::steady_clock::now();
            slot.state.resize(bytes);
            const size_t written = llama_state_seq_get_data(ctx_, slot.state.data(), slot.state.size(), 0);
            if (written == 0)
            {
                log_error("[llm] session ", active_session_id_, " kv save failed");
            }
            else
            {
                slot.state.resize(written);
                stored_sessions_.push_back(std::move(slot));
                log_info("[llm] session ", active_session_id_, " kv parked (", active_tokens_.size(), " tokens, ",
                         written, " bytes)");
            }
        }
    }
    while (static_cast<int>(stored_sessions_.size()) > keep)
    {
        auto oldest = std::min_element(stored_sessions_.begin(), stored_sessions_.end(),
                                       [](const SessionKv &a, const SessionKv &b) { return a.used < b.used; });
        log_info("[llm] session ", oldest->id, " kv evicted");
        stored_sessions_.erase(oldest);
    }
}

bool LlamaEngine::unpark_session(const std::string &session_id)
{
    auto it = std::find_if(stored_sessions_.begin(), stored_sessions_.end(),
                           [&](const SessionKv &slot) { return slot.id == session_id; });
    if (it == stored_sessions_.end())
    {
        return false;
    }
    SessionKv slot = std::move(*it);
    stored_sessions_.erase(it);
    // Clear sequence 0 before the restore so the snapshot replaces it instead of merging into it.
    trim_kv_to(0);
    const size_t loaded = llama_state_seq_set_data(ctx_, slot.state.data(), slot.state.size(), 0);
    if (loaded == 0)
    {
        log_error("[llm] session ", session_id, " kv restore failed");
        active_tokens_.clear();
        return false;
    }
    active_tokens_ = std::move(slot.tokens);
    return true;
}

void LlamaEngine::activate_session(const std::string &session_id)
{
    if (session_id == active_session_id_)
    {
        return;
    }
    park_active_session();
    active_session_id_ = session_id;
    active_tokens_.clear();
    if (!unpark_session(session_id))
    {
        trim_kv_to(0);
        log_info("[llm] session ", session_id.empty() ? "(none)" : session_id, " kv miss");
    }
    else
    {
        log_info("[llm] session ", session_id, " kv hit (", active_tokens_.size(), " tokens)");
    }
}

int LlamaEngine::get_context_size() const
{
    return ctx_ ? static_cast<int>(llama_n_ctx(ctx_)) : 0;
}

int LlamaEngine::get_used_context() const
{
    return ctx_ ? (llama_memory_seq_pos_max(llama_get_memory(ctx_), 0) + 1) : 0;
}

std::string LlamaEngine::format_messages(std::span<const ChatMessage> messages, bool add_assistant,
                                         std::span<const ChatTool> tools) const
{
    if (!templates_)
    {
        throw LlamaRuntimeError("no chat template");
    }

    common_chat_templates_inputs inputs;
    inputs.use_jinja = true;
    inputs.add_generation_prompt = add_assistant;
    inputs.enable_thinking = config_.reasoning;
    inputs.messages.reserve(messages.size());
    for (const auto &message : messages)
    {
        common_chat_msg msg;
        msg.role = message.role;
        msg.content = message.content;
        inputs.messages.push_back(std::move(msg));
    }
    inputs.tools.reserve(tools.size());
    for (const auto &tool : tools)
    {
        if (tool.name.empty())
        {
            throw LlamaRuntimeError("tool name is required");
        }
        common_chat_tool spec;
        spec.name = tool.name;
        spec.description = tool.description;
        const std::string parameters = tool.parameters.empty() ? "{\"type\":\"object\",\"properties\":{}}" : tool.parameters;
        try
        {
            const auto parsed = nlohmann::json::parse(parameters);
            if (!parsed.is_object())
            {
                throw LlamaRuntimeError("tool parameters must be a JSON object");
            }
            spec.parameters = parsed.dump();
        }
        catch (const LlamaRuntimeError &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            throw LlamaRuntimeError(std::string("tool parameters are not JSON: ") + e.what());
        }
        inputs.tools.push_back(std::move(spec));
    }

    // The first call uses the template's own tool parser when llama.cpp has one (Qwen does).
    // Templates that cannot build that parser still render; force_pure_content asks for the text only.
    auto render = [&](bool pure) {
        inputs.force_pure_content = pure;
        return common_chat_templates_apply(templates_, inputs).prompt;
    };
    try
    {
        return render(false);
    }
    catch (const std::exception &)
    {
        try
        {
            return render(true);
        }
        catch (const std::exception &e)
        {
            throw LlamaRuntimeError(std::string("failed to format chat template: ") + e.what());
        }
    }
}

std::string LlamaEngine::apply_template(std::span<const ChatMessage> messages, bool add_assistant,
                                        std::span<const ChatTool> tools) const
{
    return format_messages(messages, add_assistant, tools);
}

void LlamaEngine::decode_tokens(std::span<const int32_t> tokens, const std::function<bool()> &should_stop)
{
    if (tokens.empty())
    {
        return;
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_));
    const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_)));
    for (size_t offset = 0; offset < tokens.size();)
    {
        if (should_stop && should_stop())
        {
            throw LlamaAbort();
        }
        const int n_eval = std::min(batch_size, static_cast<int>(tokens.size() - offset));
        if (static_cast<int>(active_tokens_.size()) + n_eval > max_ctx)
        {
            throw LlamaContextFull("context size reached limit");
        }
        llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(tokens.data() + offset), n_eval);
        const int ret = llama_decode(ctx_, batch);
        if (ret != 0)
        {
            throw LlamaRuntimeError("failed to decode prompt batch");
        }
        active_tokens_.insert(active_tokens_.end(), tokens.begin() + static_cast<std::ptrdiff_t>(offset),
                              tokens.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<size_t>(n_eval)));
        offset += static_cast<size_t>(n_eval);
    }
}

std::string LlamaEngine::generate(std::string_view prompt, const LlamaRequest &request)
{
    if (!ctx_ || !vocab_ || !smpl_)
    {
        throw LlamaRuntimeError("LlamaEngine is not initialized");
    }

    activate_session(request.session_id);

    const float temperature = request.temp_override >= 0.0f ? request.temp_override : config_.temperature;
    rebuild_sampler(temperature);
    llama_perf_context_reset(ctx_);
    struct PerfGuard
    {
        llama_context *ctx = nullptr;
        ~PerfGuard()
        {
            if (ctx)
            {
                log_perf(ctx);
            }
        }
    } perf{ctx_};

    const std::vector<int32_t> prompt_tokens = tokenize_prompt(vocab_, prompt);
    if (prompt_tokens.empty())
    {
        throw LlamaRuntimeError("failed to tokenize prompt");
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(prompt_tokens.size()) >= max_ctx)
    {
        throw LlamaContextFull("context size reached limit");
    }

    // Anything past the shared token prefix is stale: a different reply, an edited message, or a
    // template that rewrote an earlier turn. Drop it before decoding the new tail.
    const size_t checkpoint = common_prefix_length(std::span<const int32_t>(active_tokens_),
                                                   std::span<const int32_t>(prompt_tokens));
    trim_kv_to(checkpoint);

    // Failed and aborted turns must not become the next prefix. The caller may ignore the partial text.
    auto rollback = [&] { trim_kv_to(checkpoint); };

    std::string response;
    try
    {
        decode_tokens(std::span<const int32_t>(prompt_tokens).subspan(checkpoint), request.should_stop);

        Utf8Util::StreamBuffer utf8_buf;
        const int max_new = effective_max_tokens(config_, request);
        int generated = 0;
        while (max_new < 0 || generated < max_new)
        {
            if (request.should_stop && request.should_stop())
            {
                throw LlamaAbort();
            }
            if (static_cast<int>(active_tokens_.size()) >= max_ctx)
            {
                throw LlamaContextFull("context size reached limit");
            }

            const llama_token new_token_id = llama_sampler_sample(smpl_, ctx_, -1);
            // EOG is not decoded and not appended. The next turn's template writes the end marker
            // (<|im_end|> and similar) into the prompt, and the token diff decodes it there.
            if (llama_vocab_is_eog(vocab_, new_token_id))
            {
                break;
            }

            char buf[256];
            int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
            std::string piece;
            if (n >= 0)
            {
                piece.assign(buf, static_cast<size_t>(n));
            }
            else
            {
                piece.resize(static_cast<size_t>(-n));
                n = llama_token_to_piece(vocab_, new_token_id, piece.data(), static_cast<int>(piece.size()), 0, true);
                if (n < 0)
                {
                    throw LlamaRuntimeError("failed to detokenize");
                }
                piece.resize(static_cast<size_t>(n));
            }
            response.append(piece);

            if (request.token_cb)
            {
                std::string ready = utf8_buf.process(piece);
                if (!ready.empty() && !request.token_cb(std::move(ready)))
                {
                    throw LlamaAbort();
                }
            }

            // Decode only after the piece was accepted. A cancel here leaves that piece in the
            // returned string and out of the cache; rollback removes the rest of the turn too.
            llama_token id = new_token_id;
            llama_batch batch = llama_batch_get_one(&id, 1);
            const int ret = llama_decode(ctx_, batch);
            if (ret != 0)
            {
                throw LlamaRuntimeError("failed to decode generation batch");
            }
            active_tokens_.push_back(new_token_id);
            ++generated;
        }

        if (request.token_cb)
        {
            std::string remaining = utf8_buf.flush();
            if (!remaining.empty())
            {
                request.token_cb(std::move(remaining));
            }
        }
        return response;
    }
    catch (const LlamaAbort &)
    {
        rollback();
        return response;
    }
    catch (...)
    {
        rollback();
        throw;
    }
}

std::string LlamaEngine::chat(std::span<const ChatMessage> messages, const LlamaRequest &request)
{
    if (!ctx_ || !vocab_ || !smpl_)
    {
        throw LlamaRuntimeError("LlamaEngine is not initialized");
    }
    // The whole formatted prompt is passed through. generate() keeps the cached token prefix
    // and decodes only the suffix, which is where the new turn and its end marker live.
    const std::string prompt = format_messages(messages, true, request.tools);
    return generate(prompt, request);
}
