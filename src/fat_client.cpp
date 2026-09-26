#include "llm/llm_engine.hpp"
#include "common/agent.hpp"
#include "common/agent_backend.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include <clocale>
#include <curl/curl.h>
#include <print>
#include <string>

namespace
{

struct CurlGlobal
{
    CurlGlobal()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobal()
    {
        curl_global_cleanup();
    }
    CurlGlobal(const CurlGlobal &) = delete;
    CurlGlobal &operator=(const CurlGlobal &) = delete;
};

void print_fat_client_usage(char **argv)
{
    std::print("\n{}Local AI Agent (Fat Client){}\n"
               "Usage:\n"
               "    {} -m <model.gguf> [options] [command]\n\n"
               "Options:\n"
               "    -m  <path>        Path to GGUF model file (required)\n"
               "    -c  <int>         Context size (default: 4096)\n"
               "    -b  <int>         Batch size (default: 2048)\n"
               "    -ngl <int>        Number of GPU layers (default: 99)\n"
               "    -t  <float>       Sampling temperature (default: 0.7)\n"
               "    --chat-template <path>  Jinja template, overrides the GGUF template\n"
               "    --no-reasoning    Disable enable_thinking in the chat template\n"
               "    --threads <int>   Generation threads, 0 = default\n"
               "    --threads-batch <int>  Prompt threads, 0 = default\n"
               "    --flash-attn auto|on|off\n"
               "    --cache-type-k <type>  --cache-type-v <type>\n"
               "    --seed <int>      Sampler seed\n"
               "    --max-tokens <int>\n"
               "    -it <int>         Max agent tool iterations per turn (default: 25)\n"
               "    --command <cmd>   Execute a single command/prompt, print result to stdout, and exit\n"
               "    -e, --exec <cmd>  Alias for --command\n"
               "    -q, --quiet, --silent Quiet mode: hide all output except the final result\n"
               "    --allow-tool <tool> Auto-approve specific tool (e.g. web_fetch) without prompt\n"
               "    --allow-tools <list> Comma-separated list of tools to auto-approve\n"
               "    --sub-agents, -sa Enable sub-agent delegation tool (default: enabled)\n"
               "    --no-sub-agents   Disable sub-agent delegation tool\n"
               "    -y, --yes, --auto-approve  Auto-approve tool execution without prompt (default: require approval)\n"
               "    -s  <prompt>      Custom system prompt (optional)\n"
               "    -h, --help        Show this help message\n",
               Color::BOLD, Color::RESET, argv[0]);
    print_slash_commands_help();
}

} // namespace

class LocalAgentBackend : public IAgentBackend {
public:
    explicit LocalAgentBackend(LlamaEngine & engine) : engine_(engine) {}

    std::string chat(std::span<const Protocol::ChatMessage> messages,
                     AgentTokenCallback token_cb = nullptr,
                     float temp_override = -1.0f) override {
        return engine_.chat(messages, token_cb, temp_override);
    }

    std::string generate(std::string_view prompt,
                         AgentTokenCallback token_cb = nullptr,
                         float temp_override = -1.0f) override {
        return engine_.generate(prompt, token_cb, temp_override);
    }

    int get_context_size() override { return engine_.get_context_size(); }
    int get_used_context() override { return engine_.get_used_context(); }
    void reset_context() override { engine_.reset(); }
    std::string get_model_name() const override { return engine_.get_config().model_path; }

private:
    LlamaEngine & engine_;
};

int main(int argc, char **argv)
{
    std::setlocale(LC_NUMERIC, "C");
    const CurlGlobal curl;

    LlamaConfig llama_config;
    AgentSessionConfig session_config;

    for (int i = 1; i < argc; i++) {
        try {
            std::string arg = argv[i];
            if (parse_llama_cli_arg(i, argc, argv, llama_config)) {
                // Keep session_config.temperature in sync with -t if specified
                session_config.temperature = llama_config.temperature;
                continue;
            }
            if (parse_agent_cli_arg(i, argc, argv, session_config, false /* -c is context size */)) {
                continue;
            }
            if (arg == "-h" || arg == "--help")
            {
                print_fat_client_usage(argv);
                return 0;
            }
            std::println(stderr, "Unknown or incomplete argument: {}", argv[i]);
            print_fat_client_usage(argv);
            return 1;
        }
        catch (const std::exception &e)
        {
            std::println(stderr, "error parsing CLI options: {}", e.what());
            print_fat_client_usage(argv);
            return 1;
        }
    }

    if (llama_config.model_path.empty())
    {
        std::println(stderr, "error: missing required model path (-m)");
        print_fat_client_usage(argv);
        return 1;
    }

    if (llama_config.n_ctx <= 0 || llama_config.n_batch <= 0 || session_config.max_iterations <= 0 ||
        session_config.temperature < 0.0f)
    {
        std::println(stderr, "error: invalid configuration parameters");
        return 1;
    }

    LlamaEngine engine;
    std::string err;
    if (!engine.init(llama_config, err))
    {
        std::println(stderr, "{}[agent] Error initializing LLM engine: {}{}", Color::RED, err, Color::RESET);
        return 1;
    }

    LocalAgentBackend backend(engine);
    return run_agent_session(backend, session_config, "Autonomous AI Agent (Fat Client Mode)");
}
