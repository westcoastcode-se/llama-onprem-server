#include "llm/llm_engine.hpp"
#include "common/agent.hpp"
#include "common/agent_backend.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include <clocale>
#include <cstdio>
#include <curl/curl.h>
#include <string>

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

static void print_fat_client_usage(int, char ** argv) {
    printf("\n%sLocal AI Agent (Fat Client)%s\n", Color::BOLD, Color::RESET);
    printf("Usage:\n");
    printf("    %s -m <model.gguf> [options] [command]\n\n", argv[0]);
    printf("Options:\n");
    printf("    -m  <path>        Path to GGUF model file (required)\n");
    printf("    -c  <int>         Context size (default: 4096)\n");
    printf("    -b  <int>         Batch size (default: 2048)\n");
    printf("    -ngl <int>        Number of GPU layers (default: 99)\n");
    printf("    -t  <float>       Sampling temperature (default: 0.7)\n");
    printf("    -it <int>         Max agent tool iterations per turn (default: 25)\n");
    printf("    --command <cmd>   Execute a single command/prompt, print result to stdout, and exit\n");
    printf("    -e, --exec <cmd>  Alias for --command\n");
    printf("    -q, --quiet, --silent Quiet mode: hide all output except the final result\n");
    printf("    --allow-tool <tool> Auto-approve specific tool (e.g. web_fetch) without prompt\n");
    printf("    --allow-tools <list> Comma-separated list of tools to auto-approve\n");
    printf("    --sub-agents, -sa Enable sub-agent delegation tool (default: enabled)\n");
    printf("    --no-sub-agents   Disable sub-agent delegation tool\n");
    printf("    -y, --yes, --auto-approve  Auto-approve tool execution without prompt (default: require approval)\n");
    printf("    -s  <prompt>      Custom system prompt (optional)\n");
    printf("    -h, --help        Show this help message\n");
    print_slash_commands_help();
}

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");
    curl_global_init(CURL_GLOBAL_DEFAULT);

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
            if (arg == "-h" || arg == "--help") {
                print_fat_client_usage(argc, argv);
                curl_global_cleanup();
                return 0;
            }
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            print_fat_client_usage(argc, argv);
            curl_global_cleanup();
            return 1;
        } catch (const std::exception & e) {
            fprintf(stderr, "error parsing CLI options: %s\n", e.what());
            print_fat_client_usage(argc, argv);
            curl_global_cleanup();
            return 1;
        }
    }

    if (llama_config.model_path.empty()) {
        fprintf(stderr, "error: missing required model path (-m)\n");
        print_fat_client_usage(argc, argv);
        curl_global_cleanup();
        return 1;
    }

    if (llama_config.n_ctx <= 0 || llama_config.n_batch <= 0 ||
        session_config.max_iterations <= 0 || session_config.temperature < 0.0f) {
        fprintf(stderr, "error: invalid configuration parameters\n");
        curl_global_cleanup();
        return 1;
    }

    LlamaEngine engine;
    std::string err;
    if (!engine.init(llama_config, err)) {
        fprintf(stderr, "%s[agent] Error initializing LLM engine: %s%s\n", Color::RED, err.c_str(), Color::RESET);
        curl_global_cleanup();
        return 1;
    }

    LocalAgentBackend backend(engine);
    int result = run_agent_session(backend, session_config, "Autonomous AI Agent (Fat Client Mode)");

    curl_global_cleanup();
    return result;
}
