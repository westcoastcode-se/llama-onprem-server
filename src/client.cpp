#include "common/agent.hpp"
#include "common/agent_backend.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include "common/net.hpp"
#include <clocale>
#include <cstdio>
#include <curl/curl.h>
#include <string>

static void print_client_usage(int, char ** argv) {
    printf("\n%sLocal AI Agent (TCP Client)%s\n", Color::BOLD, Color::RESET);
    printf("Usage:\n");
    printf("    %s [options] [command]\n\n", argv[0]);
    printf("Options:\n");
    printf("    --host <ip/host>  Server host address (default: 127.0.0.1)\n");
    printf("    -p, --port <int>  Server port (default: 8080)\n");
    printf("    -t  <float>       Sampling temperature (default: 0.7)\n");
    printf("    -it <int>         Max agent tool iterations per turn (default: 25)\n");
    printf("    -c, --command <cmd> Execute a single command/prompt, print result to stdout, and exit\n");
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

    std::string host = "127.0.0.1";
    int port = 8080;
    AgentSessionConfig config;

    for (int i = 1; i < argc; i++) {
        try {
            std::string arg = argv[i];
            if (parse_network_cli_arg(i, argc, argv, host, port)) {
                continue;
            }
            if (parse_agent_cli_arg(i, argc, argv, config, true /* allow -c for command */)) {
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                print_client_usage(argc, argv);
                curl_global_cleanup();
                return 0;
            }
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            print_client_usage(argc, argv);
            curl_global_cleanup();
            return 1;
        } catch (const std::exception & e) {
            fprintf(stderr, "error parsing CLI options: %s\n", e.what());
            print_client_usage(argc, argv);
            curl_global_cleanup();
            return 1;
        }
    }

    if (config.max_iterations <= 0 || config.temperature < 0.0f || port <= 0 || port > 65535) {
        fprintf(stderr, "error: invalid configuration parameters\n");
        curl_global_cleanup();
        return 1;
    }

    if (!config.quiet && config.single_command.empty()) {
        printf("%s[agent] Connecting to Local AI Server at %s:%d...%s\n", Color::CYAN, host.c_str(), port, Color::RESET);
    }

    std::string conn_err;
    auto sock = TcpClient::connect(host, port, conn_err);
    if (!sock || !sock->is_valid()) {
        fprintf(stderr, "%s[agent] Error connecting to server: %s%s\n", Color::RED, conn_err.c_str(), Color::RESET);
        curl_global_cleanup();
        return 1;
    }

    // Ping server handshake
    sock->send_json({{"type", "ping"}});
    nlohmann::json pong_resp;
    if (!sock->read_json(pong_resp) || pong_resp.value("type", "") != "pong") {
        fprintf(stderr, "%s[agent] Error: unexpected server handshake response%s\n", Color::RED, Color::RESET);
        curl_global_cleanup();
        return 1;
    }

    std::string model_name = pong_resp.value("model", "unknown");
    int n_ctx = pong_resp.value("n_ctx", 0);
    int used_ctx = pong_resp.value("used_ctx", 0);

    RemoteAgentBackend backend(std::move(sock), model_name, n_ctx, used_ctx);
    std::string server_info = "Server       : " + host + ":" + std::to_string(port) + " (Model: " + model_name + ")";

    int result = run_agent_session(backend, config, "Autonomous AI Agent (TCP Client Mode)", server_info);

    backend.close();
    curl_global_cleanup();
    return result;
}
