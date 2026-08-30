#include "llm/llm_engine.hpp"
#include "common/cli.hpp"
#include "common/color.hpp"
#include "common/context.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

static void print_server_usage(int, char ** argv) {
    printf("\n%sLocal AI Server%s\n", Color::BOLD, Color::RESET);
    printf("Usage:\n");
    printf("    %s -m <model.gguf> [options]\n\n", argv[0]);
    printf("Options:\n");
    printf("    -m  <path>       Path to GGUF model file (required)\n");
    printf("    -c  <int>        Context size (default: 4096)\n");
    printf("    -b  <int>        Batch size (default: 2048)\n");
    printf("    -ngl <int>       Number of GPU layers (default: 99)\n");
    printf("    -t  <float>      Sampling temperature (default: 0.7)\n");
    printf("    --host <ip/host> Host address to bind (default: 0.0.0.0)\n");
    printf("    -p, --port <int> Port to listen on (default: 8080)\n");
    printf("    -h, --help       Show this help message\n\n");
}

struct ClientThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

template <typename F>
static void handle_streaming_request(TcpSocket * client_sock,
                                     const std::string & client_ip,
                                     int client_port,
                                     const std::string & op_name,
                                     LlamaEngine & engine,
                                     std::mutex & engine_mutex,
                                     F && func) {
    std::string full_response;
    int n_ctx = 0;
    int used_ctx = 0;
    {
        std::lock_guard<std::mutex> lock(engine_mutex);
        full_response = func([&](std::string_view piece) -> bool {
            if (client_sock && client_sock->is_valid()) {
                return client_sock->send_json({{"type", "token"}, {"piece", std::string(piece)}});
            }
            return false;
        });
        n_ctx = engine.get_context_size();
        used_ctx = engine.get_used_context();
    }
    if (client_sock && client_sock->is_valid()) {
        client_sock->send_json({
            {"type", "done"},
            {"response", full_response},
            {"n_ctx", n_ctx},
            {"used_ctx", used_ctx}
        });
    }
    float pct = Context::get_usage_percentage(used_ctx, n_ctx);
    printf("%s📊 [server] Client %s:%d %s context: %d / %d tokens (%.1f%%)%s\n",
           Color::CYAN, client_ip.c_str(), client_port, op_name.c_str(), used_ctx, n_ctx, pct, Color::RESET);
}

static void handle_client(std::unique_ptr<TcpSocket> client_sock,
                          std::string client_ip,
                          int client_port,
                          LlamaEngine & engine,
                          std::mutex & engine_mutex,
                          std::shared_ptr<std::atomic<bool>> done_flag) {
    printf("%s[server] Client connected from %s:%d%s\n", Color::GREEN, client_ip.c_str(), client_port, Color::RESET);

    try {
        while (g_running && client_sock->is_valid()) {
            nlohmann::json req;
            if (!client_sock->read_json(req)) {
                break; // Client disconnected or error
            }

            std::string req_type = req.value("type", "");

            if (req_type == "ping") {
                int n_ctx = 0;
                int used_ctx = 0;
                std::string model_path;
                {
                    std::lock_guard<std::mutex> lock(engine_mutex);
                    model_path = engine.get_config().model_path;
                    n_ctx = engine.get_context_size();
                    used_ctx = engine.get_used_context();
                }
                nlohmann::json resp = {
                    {"type", "pong"},
                    {"status", "ok"},
                    {"model", model_path},
                    {"n_ctx", n_ctx},
                    {"used_ctx", used_ctx}
                };
                client_sock->send_json(resp);
            } else if (req_type == "context") {
                int n_ctx = 0;
                int used_ctx = 0;
                {
                    std::lock_guard<std::mutex> lock(engine_mutex);
                    n_ctx = engine.get_context_size();
                    used_ctx = engine.get_used_context();
                }
                nlohmann::json resp = {
                    {"type", "context"},
                    {"n_ctx", n_ctx},
                    {"used_ctx", used_ctx}
                };
                client_sock->send_json(resp);
                float pct = Context::get_usage_percentage(used_ctx, n_ctx);
                printf("%s📊 [server] Client %s:%d queried context: %d / %d tokens (%.1f%%)%s\n",
                       Color::DIM, client_ip.c_str(), client_port, used_ctx, n_ctx, pct, Color::RESET);
            } else if (req_type == "reset") {
                int n_ctx = 0;
                {
                    std::lock_guard<std::mutex> lock(engine_mutex);
                    engine.reset();
                    n_ctx = engine.get_context_size();
                }
                printf("%s[server] Context reset by client %s:%d (0 / %d tokens)%s\n",
                       Color::DIM, client_ip.c_str(), client_port, n_ctx, Color::RESET);
                client_sock->send_json({{"type", "ok"}, {"message", "context reset successful"}});
            } else if (req_type == "format") {
                auto msgs = Protocol::parse_messages(req.value("messages", nlohmann::json::array()));
                bool add_assistant = req.value("add_assistant", true);
                std::string formatted;
                {
                    std::lock_guard<std::mutex> lock(engine_mutex);
                    formatted = engine.apply_template(msgs, add_assistant);
                }
                client_sock->send_json({{"type", "formatted"}, {"text", formatted}});
            } else if (req_type == "generate") {
                std::string prompt = req.value("prompt", "");
                float temperature = req.value("temperature", engine.get_config().temperature);
                handle_streaming_request(client_sock.get(), client_ip, client_port, "generate", engine, engine_mutex,
                    [&](auto cb) { return engine.generate(prompt, cb, temperature); });
            } else if (req_type == "chat") {
                auto msgs = Protocol::parse_messages(req.value("messages", nlohmann::json::array()));
                float temperature = req.value("temperature", engine.get_config().temperature);
                handle_streaming_request(client_sock.get(), client_ip, client_port, "chat", engine, engine_mutex,
                    [&](auto cb) { return engine.chat(msgs, cb, temperature); });
            } else {
                client_sock->send_json({{"type", "error"}, {"message", "unknown request type: " + req_type}});
            }
        }
    } catch (const std::exception & e) {
        fprintf(stderr, "%s[server] Error handling client %s:%d: %s%s\n",
                Color::RED, client_ip.c_str(), client_port, e.what(), Color::RESET);
    } catch (...) {
        fprintf(stderr, "%s[server] Unknown error handling client %s:%d%s\n",
                Color::RED, client_ip.c_str(), client_port, Color::RESET);
    }

    if (done_flag) {
        done_flag->store(true);
    }

    printf("%s[server] Client disconnected: %s:%d%s\n", Color::YELLOW, client_ip.c_str(), client_port, Color::RESET);
}

int main(int argc, char ** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    LlamaConfig config;
    std::string host = "0.0.0.0";
    int port = 8080;

    for (int i = 1; i < argc; i++) {
        try {
            std::string arg = argv[i];
            if (parse_llama_cli_arg(i, argc, argv, config)) {
                continue;
            }
            if (parse_network_cli_arg(i, argc, argv, host, port)) {
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                print_server_usage(argc, argv);
                return 0;
            }
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            print_server_usage(argc, argv);
            return 1;
        } catch (const std::exception & e) {
            fprintf(stderr, "error parsing CLI options: %s\n", e.what());
            print_server_usage(argc, argv);
            return 1;
        }
    }

    if (config.model_path.empty()) {
        fprintf(stderr, "error: missing required model path (-m)\n");
        print_server_usage(argc, argv);
        return 1;
    }

    if (config.n_ctx <= 0 || config.n_batch <= 0 || config.temperature < 0.0f || port <= 0 || port > 65535) {
        fprintf(stderr, "error: invalid configuration parameters\n");
        return 1;
    }

    LlamaEngine engine;
    std::string err;
    if (!engine.init(config, err)) {
        fprintf(stderr, "%s[server] Error initializing LLM engine: %s%s\n", Color::RED, err.c_str(), Color::RESET);
        return 1;
    }

    TcpServer server;
    if (!server.listen(host, port, err)) {
        fprintf(stderr, "%s[server] Error listening on %s:%d: %s%s\n", Color::RED, host.c_str(), port, err.c_str(), Color::RESET);
        return 1;
    }

    printf("%s=======================================================%s\n", Color::CYAN, Color::RESET);
    printf("%s           Local AI TCP Server Running                 %s\n", Color::BOLD, Color::RESET);
    printf("  Listening on : %s:%d\n", host.c_str(), port);
    printf("  Model path   : %s\n", config.model_path.c_str());
    printf("  Context size : %d | GPU Layers: %d\n", config.n_ctx, config.n_gpu_layers);
    printf("%s=======================================================%s\n\n", Color::CYAN, Color::RESET);

    std::mutex engine_mutex;
    std::vector<ClientThread> client_threads;

    while (g_running) {
        // Prune finished client threads periodically
        for (auto it = client_threads.begin(); it != client_threads.end();) {
            if (it->done && it->done->load() && it->thread.joinable()) {
                it->thread.join();
                it = client_threads.erase(it);
            } else {
                ++it;
            }
        }

        struct pollfd pfd{};
        pfd.fd = server.native_handle();
        pfd.events = POLLIN;

        int poll_ret = poll(&pfd, 1, 500);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (poll_ret == 0) continue; // timeout, check g_running

        std::string client_ip;
        int client_port = 0;
        auto client_sock = server.accept(&client_ip, &client_port);
        if (client_sock && client_sock->is_valid()) {
            auto done_flag = std::make_shared<std::atomic<bool>>(false);
            std::thread t(handle_client,
                          std::move(client_sock),
                          client_ip,
                          client_port,
                          std::ref(engine),
                          std::ref(engine_mutex),
                          done_flag);
            client_threads.push_back({std::move(t), done_flag});
        }
    }

    printf("\n%s[server] Shutting down server...%s\n", Color::YELLOW, Color::RESET);
    server.close();

    for (auto & ct : client_threads) {
        if (ct.thread.joinable()) {
            ct.thread.join();
        }
    }
    client_threads.clear();

    return 0;
}
