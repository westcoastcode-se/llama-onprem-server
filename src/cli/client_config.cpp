#include "cli/client_config.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace
{

bool json_string(const nlohmann::json &value, const std::string_view key, std::string &dest, std::string &error)
{
    if (!value.is_string())
    {
        error = std::format("{} must be a string", key);
        return false;
    }
    dest = value.get<std::string>();
    return true;
}

bool json_bool(const nlohmann::json &value, const std::string_view key, bool &dest, std::string &error)
{
    if (!value.is_boolean())
    {
        error = std::format("{} must be a boolean", key);
        return false;
    }
    dest = value.get<bool>();
    return true;
}

bool json_int(const nlohmann::json &value, const std::string_view key, int &dest, std::string &error)
{
    if (!value.is_number_integer())
    {
        error = std::format("{} must be an integer", key);
        return false;
    }
    try
    {
        const auto number = value.get<std::int64_t>();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        {
            error = std::format("invalid value for {}", key);
            return false;
        }
        dest = static_cast<int>(number);
    }
    catch (const nlohmann::json::exception &)
    {
        error = std::format("invalid value for {}", key);
        return false;
    }
    return true;
}

bool apply_server(const nlohmann::json &value, ClientConfig &config, std::string &error)
{
    if (value.is_string())
    {
        config.servers = {value.get<std::string>()};
        return true;
    }
    if (!value.is_array())
    {
        error = "server must be a string or an array of strings";
        return false;
    }
    std::vector<std::string> specs;
    specs.reserve(value.size());
    for (const nlohmann::json &item : value)
    {
        if (!item.is_string())
        {
            error = "server must be a string or an array of strings";
            return false;
        }
        specs.push_back(item.get<std::string>());
    }
    config.servers = std::move(specs);
    return true;
}

bool apply_key(ClientConfig &config, const std::string_view key, const nlohmann::json &value, std::string &error)
{
    if (key == "config-file")
    {
        error = "config-file cannot be set inside a config file";
        return false;
    }
    if (key == "host")
    {
        return json_string(value, key, config.host, error);
    }
    if (key == "port")
    {
        if (!json_int(value, key, config.port, error))
        {
            return false;
        }
        if (config.port < 1 || config.port > 65535)
        {
            error = "port must be an integer from 1 to 65535";
            return false;
        }
        return true;
    }
    if (key == "json")
    {
        return json_bool(value, key, config.json, error);
    }
    if (key == "verbose")
    {
        return json_bool(value, key, config.verbose, error);
    }
    if (key == "server")
    {
        return apply_server(value, config, error);
    }
    if (key == "approval")
    {
        if (!json_string(value, key, config.approval, error))
        {
            return false;
        }
        if (config.approval != "read-only" && config.approval != "suggest" && config.approval != "auto" &&
            config.approval != "full")
        {
            error = "approval must be read-only, auto, or full";
            return false;
        }
        return true;
    }
    if (key == "resume")
    {
        return json_bool(value, key, config.resume, error);
    }
    if (key == "session")
    {
        return json_string(value, key, config.session, error);
    }
    if (key == "show-think")
    {
        return json_bool(value, key, config.show_think, error);
    }
    if (key == "debug")
    {
        return json_bool(value, key, config.debug, error);
    }
    if (key == "questions")
    {
        return json_bool(value, key, config.questions, error);
    }
    if (key == "compress-tools")
    {
        return json_bool(value, key, config.compress_tools, error);
    }
    if (key == "theme")
    {
        return json_string(value, key, config.theme, error);
    }
    error = std::format("unknown argument: {}", key);
    return false;
}

std::string apply_object(ClientConfig &config, const nlohmann::json &doc)
{
    if (!doc.is_object())
    {
        return "expected a JSON object";
    }
    for (const auto &item : doc.items())
    {
        std::string error;
        if (!apply_key(config, item.key(), item.value(), error))
        {
            return error;
        }
    }
    return {};
}

bool is_help_flag(const std::string_view arg)
{
    return arg == "-h" || arg == "--help" || arg == "--help-all";
}

} // namespace

ClientArgSplit split_client_args(const std::vector<std::string> &args)
{
    ClientArgSplit split;
    const bool help = std::ranges::any_of(args, is_help_flag);
    split.help = help;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        if (is_help_flag(arg))
        {
            split.args.push_back(arg);
            continue;
        }
        if (arg == "--config-file")
        {
            if (i + 1 >= args.size() || is_help_flag(args[i + 1]))
            {
                if (!help)
                {
                    split.error = "missing value for --config-file";
                    return split;
                }
                continue;
            }
            split.config_files.push_back(args[++i]);
            continue;
        }
        split.args.push_back(arg);
    }
    return split;
}

std::string load_client_config_file(const std::string &path, ClientConfig &config)
{
    std::ifstream input(path);
    if (!input)
    {
        return std::format("cannot read config file: {}", path);
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
    {
        return std::format("cannot read config file: {}", path);
    }
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(text);
    }
    catch (const nlohmann::json::exception &error)
    {
        return std::format("config file {}: {}", path, error.what());
    }
    const std::string error = apply_object(config, doc);
    if (!error.empty())
    {
        return std::format("config file {}: {}", path, error);
    }
    return {};
}
