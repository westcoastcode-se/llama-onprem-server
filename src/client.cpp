#include "common/agent.hpp"
#include "common/agent_backend.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include "common/net.hpp"
#include <clocale>
#include <curl/curl.h>
#include <format>
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

void print_client_usage(char **argv)
{
    std::print("\n{}Local AI Agent (TCP Client){}\n"
               "Usage:\n"
               "    {} [options] [command]\n\n"
               "Options:\n"
               "    --host <ip/host>  Server host address (default: 127.0.0.1)\n"
               "    -p, --port <int>  Server port (default: 8080)\n"
               "    -t  <float>       Sampling temperature (default: 0.7)\n"
               "    -it <int>         Max agent tool iterations per turn (default: 25)\n"
               "    -c, --command <cmd> Execute a single command/prompt, print result to stdout, and exit\n"
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

int main(int argc, char **argv)
{
    std::setlocale(LC_NUMERIC, "C");
    const CurlGlobal curl;

    std::string host = "127.0.0.1";
    int port = 8080;
    AgentSessionConfig config;

    for (int i = 1; i < argc; i++)
    {
        try
        {
            std::string arg = argv[i];
            if (parse_network_cli_arg(i, argc, argv, host, port))
            {
                continue;
            }
            if (parse_agent_cli_arg(i, argc, argv, config, true /* allow -c for command */))
            {
                continue;
            }
            if (arg == "-h" || arg == "--help")
            {
                print_client_usage(argv);
                return 0;
            }
            std::println(stderr, "Unknown or incomplete argument: {}", argv[i]);
            print_client_usage(argv);
            return 1;
        }
        catch (const std::exception &e)
        {
            std::println(stderr, "error parsing CLI options: {}", e.what());
            print_client_usage(argv);
            return 1;
        }
    }

    if (config.max_iterations <= 0 || config.temperature < 0.0f || port <= 0 || port > 65535)
    {
        std::println(stderr, "error: invalid configuration parameters");
        return 1;
    }

    if (!config.quiet && config.single_command.empty())
    {
        std::println("{}[agent] Connecting to Local AI Server at {}:{}...{}", Color::CYAN, host, port, Color::RESET);
    }

    std::string conn_err;
    auto sock = TcpClient::connect(host, port, conn_err);
    if (!sock || !sock->is_valid())
    {
        std::println(stderr, "{}[agent] Error connecting to server: {}{}", Color::RED, conn_err, Color::RESET);
        return 1;
    }

    // Ping server handshake
    sock->send_json({{"type", "ping"}});
    nlohmann::json pong_resp;
    if (!sock->read_json(pong_resp) || pong_resp.value("type", "") != "pong")
    {
        std::println(stderr, "{}[agent] Error: unexpected server handshake response{}", Color::RED, Color::RESET);
        return 1;
    }

    std::string model_name = pong_resp.value("model", "unknown");
    int n_ctx = pong_resp.value("n_ctx", 0);
    int used_ctx = pong_resp.value("used_ctx", 0);

    RemoteAgentBackend backend(std::move(sock), model_name, n_ctx, used_ctx);
    const std::string server_info = std::format("Server       : {}:{} (Model: {})", host, port, model_name);

    const int result = run_agent_session(backend, config, "Autonomous AI Agent (TCP Client Mode)", server_info);

    backend.close();
    return result;
}
