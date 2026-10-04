#pragma once

#include "server/llm/llm_engine.hpp"

#include <string>
#include <vector>

// Command line of callisto. Host and port are not model settings.
struct ServerOptions
{
    LlamaConfig config;
    std::string host{"127.0.0.1"};
    int port{8080};
    // From --api-key or the config key api-key. Empty means neither set a key.
    // main then uses CALLISTO_API_KEY, or generates a 32-character key and logs it.
    // GET /health stays open either way.
    std::string api_key;
};

// help: print usage and exit 0.
// error: print the message on stderr and exit 1.
// usage: print usage and exit 1, after error when both are set.
struct ServerArgParse
{
    bool help{false};
    bool usage{false};
    std::string error;
    ServerOptions options;
};

// args is argv without the program name.
// --config-file reads a JSON object. Keys are names such as model, context, batch,
// gpu-layers, and temperature. Longer options keep their flag names, such as top-p, port, and api-key.
// A later file overrides the keys it sets. Command-line arguments override the files.
[[nodiscard]] ServerArgParse parse_server_args(const std::vector<std::string> &args);

// 32 lowercase hex characters from 16 bytes of getentropy.
// Throws std::runtime_error when the kernel cannot supply those bytes.
[[nodiscard]] std::string generate_api_key();
