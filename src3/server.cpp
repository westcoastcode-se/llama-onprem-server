#include "server/api/errors.hpp"
#include "server/http/routes.hpp"
#include "server/jobs/jobs.hpp"
#include "server/llm/llm_engine.hpp"
#include "server/sessions/sessions.hpp"
#include <atomic>
#include <csignal>
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
            "  -t F          temperature (default 0.7)\n"
            "  --host HOST   bind host (default 0.0.0.0)\n"
            "  -p/--port N   port (default 8080)\n",
            argv0);
}
} // namespace

int main(int argc, char **argv)
{
    LlamaConfig config;
    std::string host = "0.0.0.0";
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

    std::string error;
    LlamaEngine engine = LlamaEngine::create(config, error);
    if (!error.empty())
    {
        fprintf(stderr, "failed to create engine: %s\n", error.c_str());
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

    // Custom exception handler
    svr.set_exception_handler([](const auto &, auto &res, std::exception_ptr ep) {
        try
        {
            if (ep)
            {
                std::rethrow_exception(ep);
            }
        }
        catch (const NotFound &e)
        {
            res.status = 404;
            res.set_content(error_json("not_found", e.what()), "application/json");
        }
        catch (const Busy &e)
        {
            res.status = 503;
            res.set_content(error_json("busy", e.what()), "application/json");
        }
        catch (const BadRequest &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const json::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
        catch (const std::exception &e)
        {
            res.status = 400;
            res.set_content(error_json("bad_request", e.what()), "application/json");
        }
    });

    AppState state{engine, jobs, sessions};
    register_endpoints(svr, state);

    fprintf(stderr, "[http] listening on %s:%d\n", host.c_str(), port);
    if (!svr.listen(host, port))
    {
        fprintf(stderr, "failed to listen on %s:%d\n", host.c_str(), port);
        return 1;
    }

    jobs.stop();
    g_server = nullptr;
    g_jobs = nullptr;
    return 0;
}
