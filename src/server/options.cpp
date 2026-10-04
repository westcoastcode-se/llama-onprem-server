#include "server/options.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <utility>

namespace
{

bool takes_value(const std::string_view arg)
{
    return arg == "-m" || arg == "-c" || arg == "-b" || arg == "-ngl" || arg == "-t" || arg == "--top-p" ||
           arg == "--top-k" || arg == "--min-p" || arg == "--presence-penalty" || arg == "--frequency-penalty" ||
           arg == "--repetition-penalty" || arg == "--penalty-last-n" || arg == "--seed" || arg == "--max-tokens" ||
           arg == "--threads" || arg == "--threads-batch" || arg == "--flash-attn" || arg == "--cache-type-k" ||
           arg == "--cache-type-v" || arg == "--chat-template" || arg == "--session-dir" ||
           arg == "--session-memory-mb" || arg == "--session-disk-limit" || arg == "--host" || arg == "-p" ||
           arg == "--port" || arg == "--api-key";
}

bool is_bare_flag(const std::string_view arg)
{
    return arg == "--reasoning" || arg == "--no-reasoning" || arg == "-h" || arg == "--help";
}

template <typename T> bool parse_whole(std::string_view text, T &out)
{
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }
    if (text.empty())
    {
        return false;
    }
    T value{};
    const char *first = text.data();
    const char *last = first + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last)
    {
        return false;
    }
    out = value;
    return true;
}

bool parse_float(std::string_view text, float &out)
{
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }
    if (text.empty())
    {
        return false;
    }
    float value = 0;
    const char *first = text.data();
    const char *last = first + text.size();
    const auto parsed = std::from_chars(first, last, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != last)
    {
        return false;
    }
    out = value;
    return true;
}

std::string_view trim_view(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return text;
}

bool mb_to_bytes(const uint64_t megabytes, uint64_t &bytes)
{
    constexpr uint64_t scale = 1024ull * 1024ull;
    if (megabytes > std::numeric_limits<uint64_t>::max() / scale)
    {
        return false;
    }
    bytes = megabytes * scale;
    return true;
}

// A bare number is megabytes. MB and GB are accepted in either case, with or without a space.
std::optional<uint64_t> parse_size(std::string_view text)
{
    text = trim_view(text);
    if (text.empty())
    {
        return std::nullopt;
    }
    std::size_t split = text.size();
    while (split > 0)
    {
        const unsigned char ch = static_cast<unsigned char>(text[split - 1]);
        const bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (!letter)
        {
            break;
        }
        --split;
    }
    const std::string_view number = trim_view(text.substr(0, split));
    std::string suffix(text.substr(split));
    for (char &ch : suffix)
    {
        if (ch >= 'a' && ch <= 'z')
        {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    uint64_t scale = 1024ull * 1024ull;
    if (suffix.empty() || suffix == "MB")
    {
        scale = 1024ull * 1024ull;
    }
    else if (suffix == "GB")
    {
        scale = 1024ull * 1024ull * 1024ull;
    }
    else
    {
        return std::nullopt;
    }
    uint64_t count = 0;
    if (!parse_whole(number, count) || count > std::numeric_limits<uint64_t>::max() / scale)
    {
        return std::nullopt;
    }
    return count * scale;
}

// DAYS,SIZE. Days are a whole number. SIZE is megabytes, or a value with an MB or GB suffix.
std::optional<std::pair<int, uint64_t>> parse_disk_limit(std::string_view text)
{
    text = trim_view(text);
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view days_text = trim_view(text.substr(0, comma));
    int days = 0;
    if (!parse_whole(days_text, days) || days < 0)
    {
        return std::nullopt;
    }
    const std::optional<uint64_t> bytes = parse_size(text.substr(comma + 1));
    if (!bytes)
    {
        return std::nullopt;
    }
    return std::pair<int, uint64_t>{days, *bytes};
}

bool json_string(const nlohmann::json &value, const std::string_view key, std::string &dest, ServerArgParse &status)
{
    if (!value.is_string())
    {
        status.error = std::format("{} must be a string", key);
        return false;
    }
    dest = value.get<std::string>();
    return true;
}

bool json_int(const nlohmann::json &value, const std::string_view key, int &dest, ServerArgParse &status)
{
    if (!value.is_number_integer())
    {
        status.error = std::format("{} must be an integer", key);
        return false;
    }
    try
    {
        const auto number = value.get<std::int64_t>();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        {
            status.error = std::format("invalid value for {}", key);
            return false;
        }
        dest = static_cast<int>(number);
    }
    catch (const nlohmann::json::exception &)
    {
        status.error = std::format("invalid value for {}", key);
        return false;
    }
    return true;
}

bool json_uint32(const nlohmann::json &value, const std::string_view key, uint32_t &dest, ServerArgParse &status)
{
    if (!value.is_number_integer())
    {
        status.error = std::format("{} must be an integer", key);
        return false;
    }
    try
    {
        const auto number = value.get<std::int64_t>();
        if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<uint32_t>::max())
        {
            status.error = std::format("invalid value for {}", key);
            return false;
        }
        dest = static_cast<uint32_t>(number);
    }
    catch (const nlohmann::json::exception &)
    {
        status.error = std::format("invalid value for {}", key);
        return false;
    }
    return true;
}

bool json_float(const nlohmann::json &value, const std::string_view key, float &dest, ServerArgParse &status)
{
    if (!value.is_number())
    {
        status.error = std::format("{} must be a number", key);
        return false;
    }
    try
    {
        dest = value.get<float>();
    }
    catch (const nlohmann::json::exception &)
    {
        status.error = std::format("invalid value for {}", key);
        return false;
    }
    return true;
}

bool apply_json_key(ServerOptions &options, const std::string_view key, const nlohmann::json &value,
                    ServerArgParse &status)
{
    if (key == "config-file")
    {
        status.error = "config-file cannot be set inside a config file";
        return false;
    }
    if (key == "model")
    {
        return json_string(value, key, options.config.model_path, status);
    }
    if (key == "context")
    {
        return json_int(value, key, options.config.n_ctx, status);
    }
    if (key == "batch")
    {
        return json_int(value, key, options.config.n_batch, status);
    }
    if (key == "gpu-layers")
    {
        return json_int(value, key, options.config.n_gpu_layers, status);
    }
    if (key == "temperature")
    {
        return json_float(value, key, options.config.temperature, status);
    }
    if (key == "top-p")
    {
        return json_float(value, key, options.config.top_p, status);
    }
    if (key == "top-k")
    {
        return json_int(value, key, options.config.top_k, status);
    }
    if (key == "min-p")
    {
        return json_float(value, key, options.config.min_p, status);
    }
    if (key == "presence-penalty")
    {
        return json_float(value, key, options.config.presence_penalty, status);
    }
    if (key == "frequency-penalty")
    {
        return json_float(value, key, options.config.frequency_penalty, status);
    }
    if (key == "repetition-penalty")
    {
        return json_float(value, key, options.config.repetition_penalty, status);
    }
    if (key == "penalty-last-n")
    {
        return json_int(value, key, options.config.penalty_last_n, status);
    }
    if (key == "seed")
    {
        return json_uint32(value, key, options.config.seed, status);
    }
    if (key == "max-tokens")
    {
        return json_int(value, key, options.config.max_tokens, status);
    }
    if (key == "threads")
    {
        return json_int(value, key, options.config.n_threads, status);
    }
    if (key == "threads-batch")
    {
        return json_int(value, key, options.config.n_threads_batch, status);
    }
    if (key == "flash-attn")
    {
        return json_string(value, key, options.config.flash_attn, status);
    }
    if (key == "cache-type-k")
    {
        return json_string(value, key, options.config.cache_type_k, status);
    }
    if (key == "cache-type-v")
    {
        return json_string(value, key, options.config.cache_type_v, status);
    }
    if (key == "chat-template")
    {
        return json_string(value, key, options.config.template_path, status);
    }
    if (key == "reasoning")
    {
        if (!value.is_boolean())
        {
            status.error = "reasoning must be a boolean";
            return false;
        }
        options.config.reasoning = value.get<bool>();
        return true;
    }
    if (key == "session-dir")
    {
        return json_string(value, key, options.config.session_dir, status);
    }
    if (key == "session-memory-mb")
    {
        if (value.is_string())
        {
            const std::optional<uint64_t> bytes = parse_size(value.get<std::string>());
            if (!bytes)
            {
                status.error = "invalid session-memory-mb";
                return false;
            }
            options.config.session_memory_bytes = *bytes;
            return true;
        }
        int megabytes = 0;
        if (!json_int(value, key, megabytes, status))
        {
            return false;
        }
        if (megabytes < 0 || !mb_to_bytes(static_cast<uint64_t>(megabytes), options.config.session_memory_bytes))
        {
            status.error = "invalid session-memory-mb";
            return false;
        }
        return true;
    }
    if (key == "session-disk-limit")
    {
        if (!value.is_string())
        {
            status.error = "session-disk-limit must be DAYS,SIZE";
            return false;
        }
        const std::optional<std::pair<int, uint64_t>> parsed = parse_disk_limit(value.get<std::string>());
        if (!parsed)
        {
            status.error = "invalid session-disk-limit";
            return false;
        }
        options.config.session_disk_max_age_days = parsed->first;
        options.config.session_disk_max_bytes = parsed->second;
        return true;
    }
    if (key == "host")
    {
        return json_string(value, key, options.host, status);
    }
    if (key == "port")
    {
        return json_int(value, key, options.port, status);
    }
    if (key == "api-key")
    {
        return json_string(value, key, options.api_key, status);
    }
    status.error = std::format("unknown argument: {}", key);
    status.usage = true;
    return false;
}

bool load_config_file(const std::string &path, ServerOptions &options, ServerArgParse &status)
{
    std::ifstream input(path);
    if (!input)
    {
        status.error = std::format("cannot read config file: {}", path);
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad())
    {
        status.error = std::format("cannot read config file: {}", path);
        return false;
    }
    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(text);
    }
    catch (const nlohmann::json::exception &error)
    {
        status.error = std::format("config file {}: {}", path, error.what());
        return false;
    }
    if (!doc.is_object())
    {
        status.error = std::format("config file {}: expected a JSON object", path);
        return false;
    }
    for (const auto &item : doc.items())
    {
        if (!apply_json_key(options, item.key(), item.value(), status))
        {
            status.error = std::format("config file {}: {}", path, status.error);
            return false;
        }
    }
    return true;
}

bool apply_args(ServerOptions &options, const std::vector<std::string> &args, ServerArgParse &status)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        auto missing = [&](const std::string_view name) {
            status.error = std::format("missing value for {}", name);
            return false;
        };
        auto take_int = [&](const std::string_view name, int &dest) {
            if (i + 1 >= args.size())
            {
                return missing(name);
            }
            if (!parse_whole(args[++i], dest))
            {
                status.error = std::format("invalid value for {}: {}", name, args[i]);
                return false;
            }
            return true;
        };
        auto take_float = [&](const std::string_view name, float &dest) {
            if (i + 1 >= args.size())
            {
                return missing(name);
            }
            if (!parse_float(args[++i], dest))
            {
                status.error = std::format("invalid value for {}: {}", name, args[i]);
                return false;
            }
            return true;
        };
        auto take_text = [&](const std::string_view name, std::string &dest) {
            if (i + 1 >= args.size())
            {
                return missing(name);
            }
            dest = args[++i];
            return true;
        };

        if (arg == "-m")
        {
            if (!take_text("-m", options.config.model_path))
            {
                return false;
            }
        }
        else if (arg == "-c")
        {
            if (!take_int("-c", options.config.n_ctx))
            {
                return false;
            }
        }
        else if (arg == "-b")
        {
            if (!take_int("-b", options.config.n_batch))
            {
                return false;
            }
        }
        else if (arg == "-ngl")
        {
            if (!take_int("-ngl", options.config.n_gpu_layers))
            {
                return false;
            }
        }
        else if (arg == "-t")
        {
            if (!take_float("-t", options.config.temperature))
            {
                return false;
            }
        }
        else if (arg == "--top-p")
        {
            if (!take_float("--top-p", options.config.top_p))
            {
                return false;
            }
        }
        else if (arg == "--top-k")
        {
            if (!take_int("--top-k", options.config.top_k))
            {
                return false;
            }
        }
        else if (arg == "--min-p")
        {
            if (!take_float("--min-p", options.config.min_p))
            {
                return false;
            }
        }
        else if (arg == "--presence-penalty")
        {
            if (!take_float("--presence-penalty", options.config.presence_penalty))
            {
                return false;
            }
        }
        else if (arg == "--frequency-penalty")
        {
            if (!take_float("--frequency-penalty", options.config.frequency_penalty))
            {
                return false;
            }
        }
        else if (arg == "--repetition-penalty")
        {
            if (!take_float("--repetition-penalty", options.config.repetition_penalty))
            {
                return false;
            }
        }
        else if (arg == "--penalty-last-n")
        {
            if (!take_int("--penalty-last-n", options.config.penalty_last_n))
            {
                return false;
            }
        }
        else if (arg == "--seed")
        {
            if (i + 1 >= args.size())
            {
                return missing("--seed");
            }
            if (!parse_whole(args[++i], options.config.seed))
            {
                status.error = std::format("invalid value for --seed: {}", args[i]);
                return false;
            }
        }
        else if (arg == "--max-tokens")
        {
            if (!take_int("--max-tokens", options.config.max_tokens))
            {
                return false;
            }
        }
        else if (arg == "--threads")
        {
            if (!take_int("--threads", options.config.n_threads))
            {
                return false;
            }
        }
        else if (arg == "--threads-batch")
        {
            if (!take_int("--threads-batch", options.config.n_threads_batch))
            {
                return false;
            }
        }
        else if (arg == "--flash-attn")
        {
            if (!take_text("--flash-attn", options.config.flash_attn))
            {
                return false;
            }
        }
        else if (arg == "--cache-type-k")
        {
            if (!take_text("--cache-type-k", options.config.cache_type_k))
            {
                return false;
            }
        }
        else if (arg == "--cache-type-v")
        {
            if (!take_text("--cache-type-v", options.config.cache_type_v))
            {
                return false;
            }
        }
        else if (arg == "--chat-template")
        {
            if (!take_text("--chat-template", options.config.template_path))
            {
                return false;
            }
        }
        else if (arg == "--reasoning")
        {
            options.config.reasoning = true;
        }
        else if (arg == "--no-reasoning")
        {
            options.config.reasoning = false;
        }
        else if (arg == "--session-dir")
        {
            if (!take_text("--session-dir", options.config.session_dir))
            {
                return false;
            }
        }
        else if (arg == "--session-memory-mb")
        {
            if (i + 1 >= args.size())
            {
                return missing("--session-memory-mb");
            }
            const std::string &text = args[++i];
            const std::optional<uint64_t> bytes = parse_size(text);
            if (!bytes)
            {
                status.error = std::format("invalid value for --session-memory-mb: {}", text);
                return false;
            }
            options.config.session_memory_bytes = *bytes;
        }
        else if (arg == "--session-disk-limit")
        {
            if (i + 1 >= args.size())
            {
                return missing("--session-disk-limit");
            }
            const std::optional<std::pair<int, uint64_t>> parsed = parse_disk_limit(args[++i]);
            if (!parsed)
            {
                status.error = "invalid --session-disk-limit";
                return false;
            }
            options.config.session_disk_max_age_days = parsed->first;
            options.config.session_disk_max_bytes = parsed->second;
        }
        else if (arg == "--host")
        {
            if (!take_text("--host", options.host))
            {
                return false;
            }
        }
        else if (arg == "-p" || arg == "--port")
        {
            if (!take_int(arg, options.port))
            {
                return false;
            }
        }
        else if (arg == "--api-key")
        {
            if (!take_text("--api-key", options.api_key))
            {
                return false;
            }
        }
        else if (arg == "-h" || arg == "--help")
        {
            status.help = true;
            return false;
        }
        else
        {
            status.error = std::format("unknown argument: {}", arg);
            status.usage = true;
            return false;
        }
    }
    return true;
}

} // namespace

ServerArgParse parse_server_args(const std::vector<std::string> &args)
{
    ServerArgParse result;
    std::vector<std::string> cli;
    std::vector<std::string> files;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string &arg = args[i];
        if (arg == "-h" || arg == "--help")
        {
            result.help = true;
            return result;
        }
        if (arg == "--config-file")
        {
            if (i + 1 >= args.size())
            {
                result.error = "missing value for --config-file";
                return result;
            }
            files.push_back(args[++i]);
            continue;
        }
        if (takes_value(arg))
        {
            cli.push_back(arg);
            if (i + 1 >= args.size())
            {
                break;
            }
            cli.push_back(args[++i]);
            continue;
        }
        if (!is_bare_flag(arg))
        {
            result.error = std::format("unknown argument: {}", arg);
            result.usage = true;
            return result;
        }
        cli.push_back(arg);
    }

    // Files first, then the command line, so an explicit flag replaces the file.
    for (const std::string &path : files)
    {
        if (!load_config_file(path, result.options, result))
        {
            return result;
        }
    }
    if (!apply_args(result.options, cli, result))
    {
        return result;
    }
    if (result.options.config.model_path.empty())
    {
        result.usage = true;
    }
    return result;
}

std::string generate_api_key()
{
    std::array<unsigned char, 16> bytes{};
    if (::getentropy(bytes.data(), bytes.size()) != 0)
    {
        throw std::runtime_error("getentropy failed");
    }
    std::string key(bytes.size() * 2, '\0');
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        key[i * 2] = hex[bytes[i] >> 4];
        key[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return key;
}
