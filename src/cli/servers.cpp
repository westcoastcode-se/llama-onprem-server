#include "cli/servers.hpp"

#include "common/tools.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
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

[[nodiscard]] int json_port(const nlohmann::json &item)
{
    if (!item.contains("port"))
    {
        return 8080;
    }
    const nlohmann::json &port = item.at("port");
    if (!port.is_number_integer())
    {
        throw std::runtime_error("port must be an integer from 1 to 65535");
    }
    const int value = port.get<int>();
    if (value < 1 || value > 65535)
    {
        throw std::runtime_error("port must be an integer from 1 to 65535");
    }
    return value;
}

[[nodiscard]] std::string json_string(const nlohmann::json &item, const char *key)
{
    if (!item.contains(key) || !item.at(key).is_string())
    {
        return {};
    }
    const std::string value = item.at(key).get<std::string>();
    return std::string(string_view_trim(value));
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

std::vector<ServerTarget> servers_from_json(std::string_view text)
{
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(text);
    }
    catch (const nlohmann::json::exception &error)
    {
        throw std::runtime_error(std::string("invalid JSON (") + error.what() + ")");
    }
    const nlohmann::json *list = &doc;
    if (doc.is_object())
    {
        if (!doc.contains("servers"))
        {
            throw std::runtime_error("expected a list or an object with \"servers\"");
        }
        list = &doc.at("servers");
    }
    if (!list->is_array())
    {
        throw std::runtime_error("expected a list or an object with \"servers\"");
    }
    if (list->empty())
    {
        throw std::runtime_error("server list is empty");
    }
    std::vector<ServerTarget> servers;
    servers.reserve(list->size());
    for (const nlohmann::json &item : *list)
    {
        if (item.is_string())
        {
            servers.push_back(parse_server_spec(item.get<std::string>()));
            continue;
        }
        if (!item.is_object())
        {
            throw std::runtime_error("each server must be a string or an object");
        }
        ServerTarget server;
        server.name = json_string(item, "model");
        if (server.name.empty())
        {
            server.name = json_string(item, "name");
        }
        if (item.contains("url"))
        {
            if (!item.at("url").is_string())
            {
                throw std::runtime_error("url must be a string");
            }
            const ServerTarget parsed = parse_server_spec(item.at("url").get<std::string>());
            server.host = parsed.host;
            server.port = parsed.port;
            if (server.name.empty())
            {
                server.name = parsed.name;
            }
        }
        else
        {
            server.host = json_string(item, "host");
            server.port = json_port(item);
        }
        if (server.host.empty())
        {
            throw std::runtime_error("server host is missing");
        }
        servers.push_back(std::move(server));
    }
    return servers;
}

std::filesystem::path servers_config_path()
{
    const char *home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        return {};
    }
    return std::filesystem::path(home) / ".config" / "callisto" / "servers.json";
}

std::vector<ServerTarget> load_servers_file(const std::filesystem::path &path)
{
    if (path.empty() || !std::filesystem::exists(path))
    {
        return {};
    }
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try
    {
        return servers_from_json(text);
    }
    catch (const std::exception &error)
    {
        throw std::runtime_error(std::format("{}: {}", path.string(), error.what()));
    }
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
