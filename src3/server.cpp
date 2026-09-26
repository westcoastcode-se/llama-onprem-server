#include "common/log.hpp"
#include "server/api/errors.hpp"
#include "server/http/json.hpp"
#include "server/http/routes.hpp"
#include "server/jobs/jobs.hpp"
#include "server/llm/llm_engine.hpp"
#include "server/sessions/sessions.hpp"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <httplib.h>
#include <string>

namespace
{
httplib::Server *g_server = nullptr;
Jobs *g_jobs = nullptr;
std::atomic<bool> g_stopping{false};

void on_signal(int)
{
    if (g_stopping.exchange(true))
    {
        return;
    }
    if (g_server)
    {
        g_server->stop();
    }
    if (g_jobs)
    {
        g_jobs->stop();
    }
}

void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s -m <model.gguf> [options]\n"
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
            "  --kv-sessions N        parked session KV slots including the live one (default 2)\n"
            "  --host HOST   bind host (default 0.0.0.0)\n"
            "  -p/--port N   port (default 8080)\n",
            argv0);
}
} // namespace

int main(int argc, char **argv)
{
    Logger::set_level(Logger::LEVEL_DEBUG);

    LlamaConfig config;
    string host = "127.0.0.1";
    int port = 8080;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "-m")
        {
            config.model_path = need("-m");
        }
        else if (arg == "-c")
        {
            config.n_ctx = std::stoi(need("-c"));
        }
        else if (arg == "-b")
        {
            config.n_batch = std::stoi(need("-b"));
        }
        else if (arg == "-ngl")
        {
            config.n_gpu_layers = std::stoi(need("-ngl"));
        }
        else if (arg == "-t")
        {
            config.temperature = std::stof(need("-t"));
        }
        else if (arg == "--top-p")
        {
            config.top_p = std::stof(need("--top-p"));
        }
        else if (arg == "--top-k")
        {
            config.top_k = std::stoi(need("--top-k"));
        }
        else if (arg == "--min-p")
        {
            config.min_p = std::stof(need("--min-p"));
        }
        else if (arg == "--presence-penalty")
        {
            config.presence_penalty = std::stof(need("--presence-penalty"));
        }
        else if (arg == "--repetition-penalty")
        {
            config.repetition_penalty = std::stof(need("--repetition-penalty"));
        }
        else if (arg == "--frequency-penalty")
        {
            config.frequency_penalty = std::stof(need("--frequency-penalty"));
        }
        else if (arg == "--penalty-last-n")
        {
            config.penalty_last_n = std::stoi(need("--penalty-last-n"));
        }
        else if (arg == "--seed")
        {
            config.seed = static_cast<uint32_t>(std::stoul(need("--seed")));
        }
        else if (arg == "--max-tokens")
        {
            config.max_tokens = std::stoi(need("--max-tokens"));
        }
        else if (arg == "--threads")
        {
            config.n_threads = std::stoi(need("--threads"));
        }
        else if (arg == "--threads-batch")
        {
            config.n_threads_batch = std::stoi(need("--threads-batch"));
        }
        else if (arg == "--flash-attn")
        {
            config.flash_attn = need("--flash-attn");
        }
        else if (arg == "--cache-type-k")
        {
            config.cache_type_k = need("--cache-type-k");
        }
        else if (arg == "--cache-type-v")
        {
            config.cache_type_v = need("--cache-type-v");
        }
        else if (arg == "--chat-template")
        {
            config.template_path = need("--chat-template");
        }
        else if (arg == "--reasoning")
        {
            config.reasoning = true;
        }
        else if (arg == "--no-reasoning")
        {
            config.reasoning = false;
        }
        else if (arg == "--kv-sessions")
        {
            config.kv_sessions = std::stoi(need("--kv-sessions"));
        }
        else if (arg == "--host")
        {
            host = need("--host");
        }
        else if (arg == "-p" || arg == "--port")
        {
            port = std::stoi(need(arg.c_str()));
        }
        else if (arg == "-h" || arg == "--help")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config.model_path.empty())
    {
        print_usage(argv[0]);
        return 1;
    }

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

    Jobs jobs(engine);
    Sessions sessions(jobs);
    httplib::Server svr;

    g_server = &svr;
    g_jobs = &jobs;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Timeouts
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(300, 0);
    svr.set_keep_alive_timeout(300);

    // Add simple request logging
    svr.set_pre_routing_handler([](const auto& req, auto& resp) {
        log_info(req.method, " ", req.path);
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
            send_json(res, 404, ErrorResponse{404, "not found"});
        }
        catch (const Busy &e)
        {
            send_json(res, 503, ErrorResponse{503, "busy"});
        }
        catch (const BadRequest &e)
        {
            send_json(res, 400, ErrorResponse{400, "Bad Request"});
        }
        catch (const json::exception &e)
        {
            log_error("unhandled JSON exception: ", e.what());
            send_json(res, 400, ErrorResponse{400, "Bad Request"});
        }
        catch (const std::exception &e)
        {
            log_error("unhandled exception: ", e.what());
            send_json(res, 500, ErrorResponse{500, "Internal Server Error"});
        }
    });

    AppState state{engine, jobs, sessions};
    register_endpoints(svr, state);

    log_info("server listening on ", host, ":", port);
    if (!svr.listen(host, port))
    {
        log_error("failed to listen on ", host, ":", port);
        return 1;
    }

    jobs.stop();
    g_server = nullptr;
    g_jobs = nullptr;
    return 0;
}
