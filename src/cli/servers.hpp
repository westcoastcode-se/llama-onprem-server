#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One model server. `name` is the /model label. Empty name shows host:port.
struct ServerTarget
{
    std::string name;
    std::string host;
    int port = 8080;

    [[nodiscard]] std::string label() const;
    [[nodiscard]] std::string url() const;
};

// "[name=]host:port" or "[name=]http://host:port".
[[nodiscard]] ServerTarget parse_server_spec(std::string_view spec);

// Each entry is one spec or a comma-separated list of specs.
[[nodiscard]] std::vector<ServerTarget> parse_server_list(const std::vector<std::string> &specs);

// First index for which up(index) is true. List order is the priority.
[[nodiscard]] std::optional<std::size_t> first_reachable(std::size_t count, const std::function<bool(std::size_t)> &up);
