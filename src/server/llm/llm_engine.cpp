#include "llm_engine.hpp"

#include "chat_prompt.hpp"
#include "common/log.hpp"
#include "common/xdg.hpp"
#include "context_params.hpp"
#include "kv_trim.hpp"
#include "token_offset.hpp"
#include "utf8_stream.hpp"

#include "chat.h"
#include "llama.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
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

void resolve_session_dirs(LlamaConfig &config)
{
    if (config.kv_dir.empty())
    {
        const SessionDirs dirs = default_session_dirs(config.session_dir);
        if (config.session_dir.empty())
        {
            config.session_dir = dirs.conversations.string();
        }
        config.kv_dir = dirs.kv.string();
        return;
    }
    if (config.session_dir.empty())
    {
        config.session_dir = default_session_dirs("").conversations.string();
    }
}

std::string read_text_file(const std::string &path)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
    {
        throw LlamaModelInitError(std::format("failed to read chat template from {}", path));
    }
    return {std::istreambuf_iterator<char>(in), {}};
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

void clip_message_ends(std::vector<size_t> &ends, size_t n_tokens)
{
    while (!ends.empty() && ends.back() > n_tokens)
    {
        ends.pop_back();
    }
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

void LlamaEngine::ModelDeleter::operator()(llama_model *model) const noexcept
{
    llama_model_free(model);
}

void LlamaEngine::ContextDeleter::operator()(llama_context *context) const noexcept
{
    llama_free(context);
}

void LlamaEngine::SamplerDeleter::operator()(llama_sampler *sampler) const noexcept
{
    llama_sampler_free(sampler);
}

void LlamaEngine::TemplatesDeleter::operator()(common_chat_templates *templates) const noexcept
{
    common_chat_templates_free(templates);
}

LlamaEngine::LlamaEngine() = default;

LlamaEngine::~LlamaEngine() = default;

LlamaEngine::LlamaEngine(LlamaEngine &&other) noexcept
    : config_(std::move(other.config_)), model_(std::move(other.model_)), vocab_(std::exchange(other.vocab_, nullptr)),
      ctx_(std::move(other.ctx_)), smpl_(std::move(other.smpl_)), templates_(std::move(other.templates_)),
      trim_(std::move(other.trim_)),
      active_session_id_(std::move(other.active_session_id_)), active_tokens_(std::move(other.active_tokens_)),
      message_ends_(std::move(other.message_ends_)), kv_store_(std::move(other.kv_store_))
{
}

LlamaEngine &LlamaEngine::operator=(LlamaEngine &&other) noexcept
{
    if (this != &other)
    {
        config_ = std::move(other.config_);
        model_ = std::move(other.model_);
        vocab_ = std::exchange(other.vocab_, nullptr);
        ctx_ = std::move(other.ctx_);
        smpl_ = std::move(other.smpl_);
        templates_ = std::move(other.templates_);
        trim_ = std::move(other.trim_);
        active_session_id_ = std::move(other.active_session_id_);
        active_tokens_ = std::move(other.active_tokens_);
        message_ends_ = std::move(other.message_ends_);
        kv_store_ = std::move(other.kv_store_);
    }
    return *this;
}

void LlamaEngine::rebuild_sampler(float temperature)
{
    // Built into a local chain first so a failure leaves the previous sampler in place.
    std::unique_ptr<llama_sampler, SamplerDeleter> chain(llama_sampler_chain_init(llama_sampler_chain_default_params()));
    if (!chain)
    {
        throw LlamaModelInitError("failed to create sampler chain");
    }
    const bool penalties_on = config_.repetition_penalty != 1.0f || config_.presence_penalty != 0.0f ||
                              config_.frequency_penalty != 0.0f;
    if (penalties_on && vocab_)
    {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_penalties(
                                                 llama_vocab_n_tokens(vocab_), config_.penalty_last_n, config_.repetition_penalty,
                                                 config_.frequency_penalty, config_.presence_penalty));
    }
    if (config_.top_k > 0)
    {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_top_k(config_.top_k));
    }
    if (config_.top_p < 1.0f)
    {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_top_p(config_.top_p, 1));
    }
    if (config_.min_p > 0.0f)
    {
        llama_sampler_chain_add(chain.get(), llama_sampler_init_min_p(config_.min_p, 1));
    }
    llama_sampler_chain_add(chain.get(), llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(config_.seed));
    smpl_ = std::move(chain);
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
    resolve_session_dirs(engine.config_);
    try
    {
        std::filesystem::create_directories(engine.config_.session_dir);
    }
    catch (const std::exception &error)
    {
        throw LlamaModelInitError(std::format("session directory {}: {}", engine.config_.session_dir, error.what()));
    }
    try
    {
        engine.kv_store_.open(engine.config_.kv_dir);
    }
    catch (const std::exception &error)
    {
        throw LlamaModelInitError(std::format("session kv directory {}: {}", engine.config_.kv_dir, error.what()));
    }
    log_info("[llm] session directory ", engine.config_.session_dir);
    log_info("[llm] session kv directory ", engine.config_.kv_dir);

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
    engine.model_.reset(llama_model_load_from_file(config.model_path.c_str(), model_params));
    if (!engine.model_)
    {
        throw ModelNotFound(std::format("unable to load model from {}", config.model_path));
    }

    engine.vocab_ = llama_model_get_vocab(engine.model_.get());
    engine.trim_ = make_kv_trim(engine.model_.get());
    log_info("[llm] kv trim ", engine.trim_->name());

    std::string override_src;
    if (!config.template_path.empty())
    {
        override_src = read_text_file(config.template_path);
        if (override_src.empty())
        {
            throw LlamaModelInitError(std::format("chat template is empty: {}", config.template_path));
        }
        log_info("[llm] chat template ", config.template_path);
    }
    else
    {
        log_info("[llm] chat template from model");
    }
    std::string template_src = override_src;
    if (template_src.empty())
    {
        if (const char *embedded = llama_model_chat_template(engine.model_.get(), nullptr))
        {
            template_src = embedded;
        }
    }
    // A non-empty override replaces the GGUF template. An empty string reads the template from the model.
    // Devstral is the exception: its source is patched and then passed in, so the raise is gone.
    std::string init_src = override_src;
    if (template_requires_alternating_roles(template_src))
    {
        init_src = relax_strict_role_template(template_src);
        template_src = init_src;
        engine.merge_user_turns_ = true;
        log_info("[llm] chat template allows a user turn after tools");
    }
    engine.keep_reasoning_ = template_src.empty() || template_renders_reasoning(template_src);
    engine.templates_.reset(common_chat_templates_init(engine.model_.get(), init_src).release());

    llama_context_params ctx_params = llama_context_default_params();
    apply_context_knobs(ctx_params, config.n_ctx, config.n_batch, config.n_threads, config.n_threads_batch,
                        config.flash_attn, config.cache_type_k, config.cache_type_v);

    engine.ctx_.reset(llama_init_from_model(engine.model_.get(), ctx_params));
    if (!engine.ctx_)
    {
        throw LlamaModelInitError(std::format("failed to create llama_context for {}", config.model_path));
    }

    engine.rebuild_sampler(config.temperature);
    return engine;
}

void LlamaEngine::reset()
{
    (void)trim_kv_to(0);
}

void LlamaEngine::clone_session(const std::string &from, const std::string &to)
{
    if (from.empty() || to.empty() || from == to)
    {
        return;
    }

    if (from == active_session_id_)
    {
        if (!ctx_ || active_tokens_.empty())
        {
            return;
        }
        if (!kv_store_.save(ctx_.get(), to, active_tokens_))
        {
            log_error("[llm] session ", from, " kv clone failed");
            return;
        }
        log_info("[llm] session ", from, " kv cloned to ", to, " (", active_tokens_.size(), " tokens)");
        return;
    }

    if (!kv_store_.copy(from, to))
    {
        return;
    }
    const size_t tokens = kv_store_.token_count(to).value_or(0);
    log_info("[llm] session ", from, " kv cloned to ", to, " (", tokens, " tokens)");
}

void LlamaEngine::release_session(std::string_view session_id)
{
    const std::string id(session_id);
    kv_store_.remove(id);
    if (active_session_id_ == id)
    {
        (void)trim_kv_to(0);
    }
}

bool LlamaEngine::trim_kv_to(size_t n_tokens)
{
    // Positions and active_tokens_ describe the same sequence. Trimming one without the other
    // makes the next prefix diff decode tokens on top of the wrong cache.
    if (n_tokens > active_tokens_.size())
    {
        n_tokens = active_tokens_.size();
    }
    if (!ctx_)
    {
        active_tokens_.resize(n_tokens);
        clip_message_ends(message_ends_, active_tokens_.size());
        return true;
    }
    const bool kept = trim_->trim(ctx_.get(), active_tokens_, n_tokens, active_session_id_);
    if (!kept)
    {
        message_ends_.clear();
        return false;
    }
    clip_message_ends(message_ends_, active_tokens_.size());
    return true;
}

void LlamaEngine::park_active_session()
{
    // The context can hold one sequence. The file replaces it so another session can use sequence 0.
    if (active_session_id_.empty() || !ctx_ || active_tokens_.empty())
    {
        return;
    }
    if (!kv_store_.save(ctx_.get(), active_session_id_, active_tokens_))
    {
        log_error("[llm] session ", active_session_id_, " kv save failed");
        return;
    }
    log_info("[llm] session ", active_session_id_, " kv parked (", active_tokens_.size(), " tokens)");
}

bool LlamaEngine::unpark_session(const std::string &session_id)
{
    if (!kv_store_.token_count(session_id))
    {
        return false;
    }
    // Drop whatever sequence 0 still holds. The load streams the file into that same sequence.
    (void)trim_kv_to(0);
    std::vector<int32_t> tokens;
    if (!kv_store_.load(ctx_.get(), session_id, tokens))
    {
        log_error("[llm] session ", session_id, " kv restore failed");
        active_tokens_.clear();
        message_ends_.clear();
        return false;
    }
    active_tokens_ = std::move(tokens);
    // The sequence file has no per-message ends. The next chat turn measures them.
    message_ends_.clear();
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
    message_ends_.clear();
    if (!unpark_session(session_id))
    {
        (void)trim_kv_to(0);
        log_info("[llm] session ", session_id.empty() ? "(none)" : session_id, " kv miss");
    }
    else
    {
        log_info("[llm] session ", session_id, " kv hit (", active_tokens_.size(), " tokens)");
    }
}

int LlamaEngine::get_context_size() const
{
    return ctx_ ? static_cast<int>(llama_n_ctx(ctx_.get())) : 0;
}

int LlamaEngine::get_used_context() const
{
    return ctx_ ? (llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), 0) + 1) : 0;
}

int LlamaEngine::session_token_count(std::string_view session_id) const
{
    if (session_id.empty())
    {
        return 0;
    }
    const size_t count = session_id == active_session_id_ ? active_tokens_.size()
                                                          : kv_store_.token_count(session_id).value_or(0);
    const size_t cap = static_cast<size_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(count, cap));
}

namespace
{

std::string replace_all(std::string text, std::string_view from, std::string_view to)
{
    if (from.empty())
    {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start < text.size())
    {
        const size_t at = text.find(from, start);
        if (at == std::string::npos)
        {
            out.append(text, start, std::string::npos);
            break;
        }
        out.append(text, start, at - start);
        out.append(to);
        start = at + from.size();
    }
    return out;
}

std::vector<ChatMessage> merge_consecutive_users(std::span<const ChatMessage> messages)
{
    std::vector<ChatMessage> merged;
    merged.reserve(messages.size());
    for (const ChatMessage &message : messages)
    {
        if (!merged.empty() && message.role == ChatMessage::ROLE_USER && merged.back().role == ChatMessage::ROLE_USER)
        {
            if (!message.content.empty())
            {
                if (!merged.back().content.empty())
                {
                    merged.back().content.push_back('\n');
                }
                merged.back().content += message.content;
            }
            continue;
        }
        merged.push_back(message);
    }
    return merged;
}

} // namespace

bool template_requires_alternating_roles(const std::string_view source)
{
    return source.find("conversation roles must alternate") != std::string_view::npos;
}

std::string relax_strict_role_template(std::string source)
{
    constexpr std::string_view kSingle =
        "raise_exception('After the optional system message, conversation roles must alternate user and assistant "
        "roles except for tool calls and results.')";
    constexpr std::string_view kDouble =
        "raise_exception(\"After the optional system message, conversation roles must alternate user and assistant "
        "roles except for tool calls and results.\")";
    source = replace_all(std::move(source), kSingle, "''");
    source = replace_all(std::move(source), kDouble, "''");
    return source;
}

bool template_renders_reasoning(const std::string_view source)
{
    return source.find("reasoning_content") != std::string_view::npos || source.find("<think>") != std::string_view::npos ||
           source.find("[THINK]") != std::string_view::npos;
}

std::string render_chat_prompt(const common_chat_templates *templates, const std::span<const ChatMessage> messages,
                               const std::span<const ChatTool> tools, const ChatPromptPolicy &policy)
{
    if (!templates)
    {
        throw std::runtime_error("no chat template");
    }

    const std::vector<ChatMessage> merged = policy.merge_users ? merge_consecutive_users(messages) : std::vector<ChatMessage>{};
    const std::span<const ChatMessage> turns = policy.merge_users ? std::span<const ChatMessage>{merged} : messages;

    common_chat_templates_inputs inputs;
    inputs.use_jinja = true;
    inputs.add_generation_prompt = policy.add_assistant;
    inputs.enable_thinking = policy.enable_thinking;
    inputs.messages.reserve(turns.size());
    for (const auto &message : turns)
    {
        common_chat_msg msg;
        msg.role = message.role;
        msg.content = message.content;
        // The next prompt has to reproduce the <think> block that was generated. Qwen3.5
        // cannot drop a KV suffix, so a mismatch prefills the whole prompt again.
        if (policy.keep_reasoning)
        {
            msg.reasoning_content = message.reasoning_content;
        }
        msg.tool_call_id = message.tool_call_id;
        msg.tool_name = message.tool_name;
        msg.tool_calls.reserve(message.tool_calls.size());
        for (const auto &call : message.tool_calls)
        {
            common_chat_tool_call tool_call;
            tool_call.name = call.name;
            tool_call.id = call.id;
            tool_call.arguments = call.arguments.is_string() ? call.arguments.get<std::string>() : call.arguments.dump();
            msg.tool_calls.push_back(std::move(tool_call));
        }
        inputs.messages.push_back(std::move(msg));
    }
    inputs.tools.reserve(tools.size());
    for (const auto &tool : tools)
    {
        if (tool.name.empty())
        {
            throw std::runtime_error("tool name is required");
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
                throw std::runtime_error("tool parameters must be a JSON object");
            }
            spec.parameters = parsed.dump();
        }
        catch (const std::runtime_error &)
        {
            throw;
        }
        catch (const std::exception &e)
        {
            throw std::runtime_error(std::format("tool parameters are not JSON: {}", e.what()));
        }
        inputs.tools.push_back(std::move(spec));
    }

    // The first call uses the template's own tool parser when llama.cpp has one (Qwen does).
    // Templates that cannot build that parser still render; force_pure_content asks for the text only.
    auto render = [&](bool pure) {
        inputs.force_pure_content = pure;
        return common_chat_templates_apply(templates, inputs).prompt;
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
            throw std::runtime_error(std::format("failed to format chat template: {}", e.what()));
        }
    }
}

std::string LlamaEngine::format_messages(std::span<const ChatMessage> messages, bool add_assistant,
                                         std::span<const ChatTool> tools) const
{
    ChatPromptPolicy policy;
    policy.add_assistant = add_assistant;
    policy.enable_thinking = config_.reasoning;
    policy.keep_reasoning = keep_reasoning_;
    policy.merge_users = merge_user_turns_;
    try
    {
        return render_chat_prompt(templates_.get(), messages, tools, policy);
    }
    catch (const std::exception &e)
    {
        throw LlamaRuntimeError(e.what());
    }
}

std::string LlamaEngine::apply_template(std::span<const ChatMessage> messages, bool add_assistant,
                                        std::span<const ChatTool> tools) const
{
    return format_messages(messages, add_assistant, tools);
}

void LlamaEngine::decode_tokens(std::span<const int32_t> tokens, std::move_only_function<bool()> &should_stop)
{
    if (tokens.empty())
    {
        return;
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_.get()));
    const int batch_size = std::max(1, static_cast<int>(llama_n_batch(ctx_.get())));
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
        // llama_batch_get_one does not write the tokens; the C API just lacks const.
        llama_batch batch = llama_batch_get_one(const_cast<llama_token *>(tokens.data() + offset), n_eval);
        const int ret = llama_decode(ctx_.get(), batch);
        if (ret != 0)
        {
            throw LlamaRuntimeError("failed to decode prompt batch");
        }
        active_tokens_.append_range(tokens.subspan(offset, static_cast<size_t>(n_eval)));
        offset += static_cast<size_t>(n_eval);
    }
}

std::vector<size_t> LlamaEngine::message_token_ends(std::span<const ChatMessage> messages,
                                                           std::span<const ChatTool> tools,
                                                           std::span<const int32_t> prompt_tokens) const
{
    std::vector<size_t> ends;
    size_t start = 0;
    // A stored end that is still a prefix of this prompt belongs to an unchanged message.
    if (!message_ends_.empty())
    {
        const size_t last = message_ends_.back();
        if (last <= prompt_tokens.size() && last <= active_tokens_.size() &&
            std::equal(active_tokens_.begin(), active_tokens_.begin() + static_cast<std::ptrdiff_t>(last),
                       prompt_tokens.begin()))
        {
            ends = message_ends_;
            start = message_ends_.size();
        }
    }

    for (size_t i = start; i < messages.size(); ++i)
    {
        // add_assistant false: the generation prompt is not part of any message.
        const std::string text = format_messages(messages.subspan(0, i + 1), false, tools);
        const std::vector<int32_t> tokens = tokenize_prompt(vocab_, text);
        if (tokens.size() > prompt_tokens.size())
        {
            break;
        }
        if (!std::equal(tokens.begin(), tokens.end(), prompt_tokens.begin()))
        {
            break;
        }
        if (!ends.empty() && tokens.size() < ends.back())
        {
            break;
        }
        ends.push_back(tokens.size());
    }
    return ends;
}

std::string LlamaEngine::generate(std::string_view prompt, const LlamaRequest &request)
{
    if (!ctx_ || !vocab_ || !smpl_)
    {
        throw LlamaRuntimeError("LlamaEngine is not initialized");
    }
    const std::vector<int32_t> prompt_tokens = tokenize_prompt(vocab_, prompt);
    return generate_tokens(prompt_tokens, {}, request);
}

std::string LlamaEngine::generate_tokens(std::span<const int32_t> prompt_tokens, std::span<const size_t> message_ends,
                                         const LlamaRequest &request)
{
    if (!ctx_ || !vocab_ || !smpl_)
    {
        throw LlamaRuntimeError("LlamaEngine is not initialized");
    }

    activate_session(request.session_id);

    const float temperature = request.temp_override >= 0.0f ? request.temp_override : config_.temperature;
    rebuild_sampler(temperature);
    llama_perf_context_reset(ctx_.get());
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
    } perf{ctx_.get()};

    if (prompt_tokens.empty())
    {
        throw LlamaRuntimeError("failed to tokenize prompt");
    }
    const int max_ctx = static_cast<int>(llama_n_ctx(ctx_.get()));
    if (static_cast<int>(prompt_tokens.size()) >= max_ctx)
    {
        throw LlamaContextFull("context size reached limit");
    }

    // Stored ends name the unchanged messages. The scan after the last matching end covers the
    // generated tail. A model that cannot drop a suffix clears the sequence instead.
    size_t checkpoint = checkpoint_from_offsets(active_tokens_, prompt_tokens, message_ends_);
    if (!trim_kv_to(checkpoint))
    {
        checkpoint = 0;
    }

    // Failed and aborted turns must not become the next prefix. The caller may ignore the partial text.
    auto rollback = [&] { (void)trim_kv_to(checkpoint); };

    std::string response;
    try
    {
        decode_tokens(std::span<const int32_t>(prompt_tokens).subspan(checkpoint), request.should_stop);
        if (request.on_prompt)
        {
            request.on_prompt();
        }

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

            const llama_token new_token_id = llama_sampler_sample(smpl_.get(), ctx_.get(), -1);
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
            const int ret = llama_decode(ctx_.get(), batch);
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
        if (!message_ends.empty())
        {
            message_ends_.assign(message_ends.begin(), message_ends.end());
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
    // Offsets stay on this session. The client sends messages and never a token position.
    activate_session(request.session_id);
    const std::string prompt = format_messages(messages, true, request.tools);
    const std::vector<int32_t> prompt_tokens = tokenize_prompt(vocab_, prompt);
    const std::vector<size_t> ends = message_token_ends(messages, request.tools, prompt_tokens);
    return generate_tokens(prompt_tokens, ends, request);
}
