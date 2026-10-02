#pragma once

#include <string>
#include <vector>

// Settings that callisto_cli flags also set. The positional task and subcommand
// arguments stay on the command line.
struct ClientConfig
{
    std::string host{"127.0.0.1"};
    int port{8080};
    bool json{false};
    bool verbose{false};
    // Each entry is one --server value, "[model=]host:port".
    std::vector<std::string> servers;
    std::string approval{"read-only"};
    bool resume{false};
    std::string session;
    bool show_think{true};
    bool debug{false};
    bool questions{true};
    bool compress_tools{true};
    std::string theme;
};

// argv without the program name, after --config-file pairs are removed.
// help is set when -h, --help, or --help-all is present. A missing --config-file
// value sets error. config_files is in the order given.
struct ClientArgSplit
{
    bool help{false};
    std::string error;
    std::vector<std::string> config_files;
    std::vector<std::string> args;
};

[[nodiscard]] ClientArgSplit split_client_args(const std::vector<std::string> &args);

// Reads one JSON object and applies the keys it sets. An empty string means success.
// A later file overrides the keys it sets. Unknown keys and bad types name the file.
[[nodiscard]] std::string load_client_config_file(const std::string &path, ClientConfig &config);
