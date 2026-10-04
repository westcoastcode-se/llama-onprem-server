#include "common/log.hpp"
#include "server/agent/model_adapter.hpp"
#include "server/api/errors.hpp"
#include "server/http/json.hpp"
#include "server/http/routes.hpp"
#include "server/jobs/jobs.hpp"
#include "server/llm/llm_engine.hpp"
#include "server/options.hpp"
#include "server/responses/responses.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <format>
#include <httplib.h>
#include <poll.h>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

int g_wake_fd = -1;
volatile sig_atomic_t g_stop_flag = 0;

void on_signal(int)
{
    if (g_stop_flag)
    {
        return;
    }
    g_stop_flag = 1;
    if (g_wake_fd >= 0)
    {
        const char byte = 1;
        const ssize_t n = ::write(g_wake_fd, &byte, 1);
        (void)n;
    }
}

void print_usage(const char *argv0)
{
    std::println(stderr,
                 "Usage: {} -m <model.gguf> [options]\n"
                 "  -m PATH       model path (required)\n"
                 "  -c N          context size (default 4096)\n"
                 "  -b N          batch size (default 2048)\n"
                 "  -ngl N        GPU layers (default 99)\n"
                 "  -t F          temperature (default 1.0)\n"
                 "  --top-p F     nucleus sampling (default 0.95)\n"
                 "  --top-k N     top-k sampling (default 20, 0 = off)\n"
                 "  --min-p F     min-p sampling (default 0, 0 = off)\n"
                 "  --presence-penalty F   (default 0)\n"
                 "  --frequency-penalty F  (default 0)\n"
                 "  --repetition-penalty F (default 1.0 = off)\n"
                 "  --penalty-last-n N     penalty window (default 64)\n"
                 "  --seed N      sampler seed (default random)\n"
                 "  --max-tokens N         cap new tokens, -1 = context (default -1)\n"
                 "  --threads N            generation threads, 0 = default\n"
                 "  --threads-batch N      prompt threads, 0 = default\n"
                 "  --flash-attn auto|on|off (default auto)\n"
                 "  --cache-type-k TYPE    KV cache K type (default f16)\n"
                 "  --cache-type-v TYPE    KV cache V type (default f16)\n"
                 "  --chat-template PATH   Jinja template, overrides the GGUF template\n"
                 "  --reasoning / --no-reasoning   enable_thinking (default on)\n"
                 "  --session-dir PATH     parked KV files. Default is\n"
                 "                         $XDG_CACHE_HOME/callisto/sessions. PATH stores them there.\n"
                 "  --session-memory-mb SIZE  park sessions in RAM (default 0 = disk only).\n"
                 "                         A number is megabytes. MB and GB are accepted.\n"
                 "  --session-disk-limit DAYS,SIZE\n"
                 "                         on start, on shutdown, and after each disk write, delete files\n"
                 "                         older than DAYS while the directory is larger than SIZE.\n"
                 "                         SIZE is megabytes, or a value with an MB or GB suffix.\n"
                 "  --config-file PATH  JSON object of these settings\n"
                 "  --api-key KEY require Authorization: Bearer KEY. Empty leaves the API open.\n"
                 "                CALLISTO_API_KEY is used when this flag and the config file leave it empty.\n"
                 "                GET /health never checks the key.\n"
                 "  --host HOST   bind host (default 127.0.0.1)\n"
                 "  -p/--port N   port (default 8080)\n"
                 "\n"
                 "JSON keys use these flag names. Short flags are model, context, batch, gpu-layers, and temperature.\n"
                 "reasoning is true or false. api-key is a string.\n"
                 "session-memory-mb is megabytes, or a string such as 512MB or 1GB.\n"
                 "session-disk-limit is DAYS,SIZE, for example 14,512MB or 14,1GB.\n"
                 "A later --config-file overrides the keys it sets.\n"
                 "Arguments on the command line override the file. The environment variable does not override them.",
                 argv0);
}
} // namespace

int main(int argc, char **argv)
{
    Logger::set_level(Logger::LEVEL_DEBUG);

    std::vector<std::string> args;
    if (argc > 1)
    {
        args.assign(argv + 1, argv + argc);
    }
    const ServerArgParse parsed = parse_server_args(args);
    if (parsed.help)
    {
        print_usage(argv[0]);
        return 0;
    }
    if (!parsed.error.empty())
    {
        std::println(stderr, "{}", parsed.error);
    }
    if (parsed.usage)
    {
        print_usage(argv[0]);
    }
    if (!parsed.error.empty() || parsed.usage)
    {
        return 1;
    }
    ServerOptions options = parsed.options;
    if (options.api_key.empty())
    {
        if (const char *from_env = std::getenv("CALLISTO_API_KEY"))
        {
            options.api_key = from_env;
        }
    }
    const LlamaConfig &config = options.config;
    const std::string &host = options.host;
    const int port = options.port;
    const std::string api_key = options.api_key;

    LlamaEngine engine;
    try
    {
        engine = LlamaEngine::create(config);
    }
    catch (std::exception &e)
    {
        log_error("Failed to create LLamaEngine: ", e.what());
        return 1;
    }

    const auto adapter = make_model_adapter(config.model_path, config.template_path);
    log_info("[llm] assistant format ", adapter->name());
    Jobs jobs(engine, *adapter);
    httplib::Server svr;

    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0)
    {
        log_error("failed to create shutdown pipe");
        return 1;
    }
    g_wake_fd = wake[1];
    std::jthread stopper([&svr, &jobs, read_fd = wake[0]](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            pollfd pfd{};
            pfd.fd = read_fd;
            pfd.events = POLLIN;
            const int rc = ::poll(&pfd, 1, 200);
            if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
            {
                jobs.request_shutdown();
                svr.stop();
                return;
            }
        }
    });

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Timeouts
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(300, 0);
    svr.set_keep_alive_timeout(300);

    if (!api_key.empty())
    {
        log_info("API key required for every route except GET /health");
    }

    // Reject a missing or wrong bearer token before the handler runs. The header is not logged.
    svr.set_pre_routing_handler([api_key](const auto &req, auto &res) {
        const bool health = req.method == "GET" && req.path == "/health";
        if (!api_key.empty() && !health && !authorization_matches(api_key, req.get_header_value("Authorization")))
        {
            send_openai_error(res, 401, "invalid_request_error", "invalid api key", "invalid_api_key");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (Logger::is_level(Logger::LEVEL_DEBUG))
        {
            log_info("Method: ", req.method, " Path: ", req.path, " Body: ", req.body);
        }
        else
        {
            log_info("Method: ", req.method, " Path: ", req.path);
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Custom exception handler
    svr.set_exception_handler([](const auto &, auto &res, const std::exception_ptr& ep) {
        try
        {
            if (ep)
            {
                std::rethrow_exception(ep);
            }
        }
        catch (const NotFound &e)
        {
            send_json(res, 404, ErrorResponse{404, e.what()});
        }
        catch (const Busy &e)
        {
            send_json(res, 503, ErrorResponse{503, e.what()});
        }
        catch (const BadRequest &e)
        {
            send_json(res, 400, ErrorResponse{400, e.what()});
        }
        catch (const nlohmann::json::exception &e)
        {
            log_error("unhandled JSON exception: ", e.what());
            send_json(res, 400, ErrorResponse{400, e.what()});
        }
        catch (const std::invalid_argument &)
        {
            send_json(res, 400, ErrorResponse{400, "invalid id"});
        }
        catch (const std::out_of_range &)
        {
            send_json(res, 400, ErrorResponse{400, "invalid id"});
        }
        catch (const std::exception &e)
        {
            log_error("unhandled exception: ", e.what());
            send_json(res, 500, ErrorResponse{500, e.what()});
        }
    });

    AppState state{engine, jobs};
    register_endpoints(svr, state);

    log_info("server listening on ", host, ":", port);
    if (!svr.listen(host, port))
    {
        log_error("failed to listen on ", host, ":", port);
        jobs.stop();
        return 1;
    }

    jobs.stop();
    stopper.request_stop();
    stopper.join();
    g_wake_fd = -1;
    ::close(wake[0]);
    ::close(wake[1]);
    return 0;
}
