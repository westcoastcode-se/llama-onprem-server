#pragma once

#include "server/llm/llm_engine.hpp"

#include <string>
#include <vector>

// Command line of callisto_server. Host and port are not model settings.
struct ServerOptions
{
    LlamaConfig config;
    std::string host{"127.0.0.1"};
    int port{8080};
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
// --config-file reads a JSON object. Keys are the argument names without leading dashes.
// A later file overrides the keys it sets. Command-line arguments override the files.
[[nodiscard]] ServerArgParse parse_server_args(const std::vector<std::string> &args);
