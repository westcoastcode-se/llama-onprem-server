#include "cli/servers.hpp"

#include "common/tools.hpp"

#include <format>
#include <stdexcept>

namespace
{

[[nodiscard]] int parse_port(std::string_view text)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string_view::npos)
    {
        throw std::runtime_error("port must be an integer from 1 to 65535");
    }
    try
    {
        const int port = std::stoi(std::string(text));
        if (port >= 1 && port <= 65535)
        {
            return port;
        }
    }
    catch (const std::exception &)
    {
    }
    throw std::runtime_error("port must be an integer from 1 to 65535");
}

[[nodiscard]] ServerTarget parse_endpoint(std::string_view body)
{
    body = string_view_trim(body);
    if (body.starts_with("https://"))
    {
        throw std::runtime_error("server url must be http");
    }
    if (body.starts_with("http://"))
    {
        body.remove_prefix(std::string_view("http://").size());
    }
    if (const auto slash = body.find('/'); slash != std::string_view::npos)
    {
        if (slash + 1 != body.size())
        {
            throw std::runtime_error("server url must be host and port");
        }
        body = body.substr(0, slash);
    }
    ServerTarget server;
    const auto colon = body.rfind(':');
    if (colon == std::string_view::npos)
    {
        server.host = std::string(body);
    }
    else
    {
        server.host = std::string(body.substr(0, colon));
        server.port = parse_port(body.substr(colon + 1));
    }
    if (server.host.empty() || server.host.find(' ') != std::string::npos)
    {
        throw std::runtime_error("server host is missing");
    }
    return server;
}

} // namespace

std::string ServerTarget::label() const
{
    if (!name.empty())
    {
        return name;
    }
    return std::format("{}:{}", host, port);
}

std::string ServerTarget::url() const
{
    return std::format("http://{}:{}", host, port);
}

ServerTarget parse_server_spec(std::string_view spec)
{
    spec = string_view_trim(spec);
    if (spec.empty())
    {
        throw std::runtime_error("server spec is empty");
    }
    std::string name;
    std::string_view body = spec;
    const auto scheme = spec.find("://");
    const auto eq = spec.find('=');
    if (eq != std::string_view::npos && (scheme == std::string_view::npos || eq < scheme))
    {
        name = string_view_trim(spec.substr(0, eq));
        body = spec.substr(eq + 1);
        if (name.empty())
        {
            throw std::runtime_error("server name is empty");
        }
    }
    ServerTarget server = parse_endpoint(body);
    server.name = std::move(name);
    return server;
}

std::vector<ServerTarget> parse_server_list(const std::vector<std::string> &specs)
{
    std::vector<ServerTarget> servers;
    for (const std::string &spec : specs)
    {
        std::size_t start = 0;
        while (start <= spec.size())
        {
            const std::size_t comma = spec.find(',', start);
            const std::string_view part = string_view_trim(std::string_view(spec).substr(
                start, comma == std::string::npos ? std::string_view::npos : comma - start));
            if (!part.empty())
            {
                servers.push_back(parse_server_spec(part));
            }
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
    }
    if (servers.empty())
    {
        throw std::runtime_error("server list is empty");
    }
    return servers;
}

std::optional<std::size_t> first_reachable(std::size_t count, const std::function<bool(std::size_t)> &up)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        if (up(index))
        {
            return index;
        }
    }
    return std::nullopt;
}
