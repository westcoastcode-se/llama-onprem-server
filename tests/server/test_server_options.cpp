#include "common/defer.hpp"
#include "server/options.hpp"
#include "../tests.hpp"

#include <cstdint>
#include <string>

namespace
{

std::string config_path(const std::filesystem::path &dir, const char *name)
{
    return (dir / name).string();
}

} // namespace

/**
 * Every server argument can be set from a JSON config file.
 */
static int test_config_file_sets_every_argument()
{
    const auto dir = make_temp_dir("server-args");
    defer(std::filesystem::remove_all(dir));
    const auto path = config_path(dir, "server.json");
    write_test_file(path, R"({
        "model": "/tmp/my model.gguf",
        "context": 32768,
        "batch": 1024,
        "gpu-layers": 40,
        "temperature": 0.5,
        "top-p": 0.5,
        "top-k": 40,
        "min-p": 0.25,
        "presence-penalty": 0.5,
        "frequency-penalty": 0.5,
        "repetition-penalty": 1.5,
        "penalty-last-n": 128,
        "seed": 7,
        "max-tokens": 64,
        "threads": 4,
        "threads-batch": 2,
        "flash-attn": "on",
        "cache-type-k": "q8_0",
        "cache-type-v": "q4_0",
        "chat-template": "/tmp/chat#template.jinja",
        "reasoning": false,
        "session-dir": "/tmp/session dir",
        "session-memory-mb": 256,
        "session-disk-limit": "14,32",
        "host": "0.0.0.0",
        "port": 9090,
        "api-key": "file-secret"
    })");

    const ServerArgParse parsed = parse_server_args({"--config-file", path});
    assertTrue(parsed.error.empty());
    assertTrue(!parsed.help);
    assertTrue(!parsed.usage);
    const LlamaConfig &config = parsed.options.config;
    assertEquals(std::string("/tmp/my model.gguf"), config.model_path);
    assertEquals(32768, config.n_ctx);
    assertEquals(1024, config.n_batch);
    assertEquals(40, config.n_gpu_layers);
    assertTrue(config.temperature == 0.5f);
    assertTrue(config.top_p == 0.5f);
    assertEquals(40, config.top_k);
    assertTrue(config.min_p == 0.25f);
    assertTrue(config.presence_penalty == 0.5f);
    assertTrue(config.frequency_penalty == 0.5f);
    assertTrue(config.repetition_penalty == 1.5f);
    assertEquals(128, config.penalty_last_n);
    assertEquals(static_cast<uint32_t>(7), config.seed);
    assertEquals(64, config.max_tokens);
    assertEquals(4, config.n_threads);
    assertEquals(2, config.n_threads_batch);
    assertEquals(std::string("on"), config.flash_attn);
    assertEquals(std::string("q8_0"), config.cache_type_k);
    assertEquals(std::string("q4_0"), config.cache_type_v);
    assertEquals(std::string("/tmp/chat#template.jinja"), config.template_path);
    assertTrue(!config.reasoning);
    assertEquals(std::string("/tmp/session dir"), config.session_dir);
    assertEquals(static_cast<uint64_t>(256) * 1024 * 1024, config.session_memory_bytes);
    assertEquals(14, config.session_disk_max_age_days);
    assertEquals(static_cast<uint64_t>(32) * 1024 * 1024, config.session_disk_max_bytes);
    assertEquals(std::string("0.0.0.0"), parsed.options.host);
    assertEquals(9090, parsed.options.port);
    assertEquals(std::string("file-secret"), parsed.options.api_key);
    return EXIT_SUCCESS;
}

/**
 * Omitting flags keeps the built-in defaults. -m is still required.
 */
static int test_server_args_keep_defaults()
{
    const ServerArgParse missing = parse_server_args({});
    assertTrue(missing.error.empty());
    assertTrue(missing.usage);
    assertTrue(!missing.help);

    const ServerArgParse parsed = parse_server_args({"-m", "model.gguf"});
    assertTrue(parsed.error.empty());
    assertTrue(!parsed.usage);
    const LlamaConfig &config = parsed.options.config;
    assertEquals(std::string("model.gguf"), config.model_path);
    assertEquals(4096, config.n_ctx);
    assertEquals(2048, config.n_batch);
    assertEquals(99, config.n_gpu_layers);
    assertTrue(config.temperature == 1.0f);
    assertTrue(config.top_p == 0.95f);
    assertEquals(20, config.top_k);
    assertTrue(config.min_p == 0.0f);
    assertTrue(config.presence_penalty == 0.0f);
    assertTrue(config.frequency_penalty == 0.0f);
    assertTrue(config.repetition_penalty == 1.0f);
    assertEquals(64, config.penalty_last_n);
    assertEquals(static_cast<uint32_t>(0xFFFFFFFFu), config.seed);
    assertEquals(-1, config.max_tokens);
    assertEquals(0, config.n_threads);
    assertEquals(0, config.n_threads_batch);
    assertEquals(std::string("auto"), config.flash_attn);
    assertEquals(std::string("f16"), config.cache_type_k);
    assertEquals(std::string("f16"), config.cache_type_v);
    assertTrue(config.template_path.empty());
    assertTrue(config.reasoning);
    assertTrue(config.session_dir.empty());
    assertTrue(config.kv_dir.empty());
    assertEquals(static_cast<uint64_t>(0), config.session_memory_bytes);
    assertEquals(-1, config.session_disk_max_age_days);
    assertEquals(static_cast<uint64_t>(0), config.session_disk_max_bytes);
    assertEquals(std::string("127.0.0.1"), parsed.options.host);
    assertEquals(8080, parsed.options.port);
    assertTrue(parsed.options.api_key.empty());
    return EXIT_SUCCESS;
}

/**
 * Command-line flags replace the file, wherever --config-file sits.
 * A later file replaces only the keys it sets. -1 is a value, not another flag.
 */
static int test_command_line_overrides_config_file()
{
    const auto dir = make_temp_dir("server-args-override");
    defer(std::filesystem::remove_all(dir));
    const auto first = config_path(dir, "first.json");
    const auto second = config_path(dir, "second.json");
    write_test_file(first, R"({
        "model": "from-file.gguf",
        "context": 111,
        "host": "1.1.1.1",
        "port": 2222,
        "reasoning": false,
        "api-key": "from-file"
    })");
    write_test_file(second, R"({
        "context": 222,
        "port": 3333
    })");

    const ServerArgParse parsed = parse_server_args({
        "-m",
        "from-cli.gguf",
        "-c",
        "1",
        "--config-file",
        first,
        "--config-file",
        second,
        "-p",
        "9",
        "--reasoning",
        "--max-tokens",
        "-1",
        "--api-key",
        "from-cli",
    });
    assertTrue(parsed.error.empty());
    assertEquals(std::string("from-cli.gguf"), parsed.options.config.model_path);
    assertEquals(1, parsed.options.config.n_ctx);
    assertEquals(std::string("1.1.1.1"), parsed.options.host);
    assertEquals(9, parsed.options.port);
    assertTrue(parsed.options.config.reasoning);
    assertEquals(-1, parsed.options.config.max_tokens);

    const ServerArgParse from_files = parse_server_args({"-m", "cli.gguf", "--config-file", first, "--config-file", second});
    assertEquals(222, from_files.options.config.n_ctx);
    assertEquals(3333, from_files.options.port);
    assertTrue(!from_files.options.config.reasoning);
    assertEquals(std::string("1.1.1.1"), from_files.options.host);
    assertEquals(std::string("from-file"), from_files.options.api_key);
    assertEquals(std::string("from-cli"), parsed.options.api_key);

    const ServerArgParse missing_key = parse_server_args({"-m", "model.gguf", "--api-key"});
    assertEquals(std::string("missing value for --api-key"), missing_key.error);
    return EXIT_SUCCESS;
}

/**
 * JSON strings keep spaces and flag-like text. A command-line value is not parsed as a flag.
 */
static int test_config_file_strings_and_flag_like_values()
{
    const auto dir = make_temp_dir("server-args-strings");
    defer(std::filesystem::remove_all(dir));
    const auto path = config_path(dir, "strings.json");
    write_test_file(path, R"({
        "model": "model.gguf",
        "host": "--config-file",
        "chat-template": "a\"b",
        "session-dir": "a\\b"
    })");

    const ServerArgParse parsed = parse_server_args({"--config-file", path});
    assertTrue(parsed.error.empty());
    assertEquals(std::string("model.gguf"), parsed.options.config.model_path);
    assertEquals(std::string("--config-file"), parsed.options.host);
    assertEquals(std::string("a\"b"), parsed.options.config.template_path);
    assertEquals(std::string("a\\b"), parsed.options.config.session_dir);

    const ServerArgParse on_cli = parse_server_args({"--host", "--help", "-m", "model.gguf"});
    assertTrue(on_cli.error.empty());
    assertTrue(!on_cli.help);
    assertEquals(std::string("--help"), on_cli.options.host);
    return EXIT_SUCCESS;
}

/**
 * Help stops the parse. A broken later flag is not reported. An earlier unknown flag is.
 */
static int test_server_help_and_unknown_arguments()
{
    const ServerArgParse help = parse_server_args({"--help", "--config-file"});
    assertTrue(help.help);
    assertTrue(help.error.empty());
    assertTrue(!help.usage);

    const ServerArgParse short_help = parse_server_args({"-h"});
    assertTrue(short_help.help);

    const ServerArgParse unknown = parse_server_args({"--bogus", "--help"});
    assertTrue(!unknown.help);
    assertTrue(unknown.usage);
    assertTrue(unknown.error.find("unknown argument: --bogus") != std::string::npos);

    const ServerArgParse missing_flag = parse_server_args({"-m"});
    assertTrue(!missing_flag.usage);
    assertEquals(std::string("missing value for -m"), missing_flag.error);

    const ServerArgParse missing_file = parse_server_args({"--config-file"});
    assertEquals(std::string("missing value for --config-file"), missing_file.error);

    const ServerArgParse bad_number = parse_server_args({"-m", "model.gguf", "-c", "12x"});
    assertTrue(bad_number.error.find("invalid value for -c: 12x") != std::string::npos);
    assertTrue(!bad_number.usage);

    const ServerArgParse old_kv = parse_server_args({"-m", "model.gguf", "--kv-sessions", "2"});
    assertTrue(old_kv.usage);
    assertTrue(old_kv.error.find("unknown argument: --kv-sessions") != std::string::npos);

    const ServerArgParse old_cache = parse_server_args({"-m", "model.gguf", "--session-cache-size", "8G"});
    assertTrue(old_cache.usage);
    assertTrue(old_cache.error.find("unknown argument: --session-cache-size") != std::string::npos);

    const ServerArgParse bad_memory = parse_server_args({"-m", "model.gguf", "--session-memory-mb", "-1"});
    assertTrue(bad_memory.error.find("invalid value for --session-memory-mb") != std::string::npos);

    const ServerArgParse bad_limit = parse_server_args({"-m", "model.gguf", "--session-disk-limit", "7"});
    assertEquals(std::string("invalid --session-disk-limit"), bad_limit.error);

    const ServerArgParse memory_gb = parse_server_args({"-m", "model.gguf", "--session-memory-mb", "1GB"});
    assertTrue(memory_gb.error.empty());
    assertEquals(static_cast<uint64_t>(1024) * 1024 * 1024, memory_gb.options.config.session_memory_bytes);

    const ServerArgParse memory_mb = parse_server_args({"-m", "model.gguf", "--session-memory-mb", "512 mb"});
    assertTrue(memory_mb.error.empty());
    assertEquals(static_cast<uint64_t>(512) * 1024 * 1024, memory_mb.options.config.session_memory_bytes);

    const ServerArgParse memory_bare = parse_server_args({"-m", "model.gguf", "--session-memory-mb", "4"});
    assertTrue(memory_bare.error.empty());
    assertEquals(static_cast<uint64_t>(4) * 1024 * 1024, memory_bare.options.config.session_memory_bytes);

    const ServerArgParse disk_gb = parse_server_args({"-m", "model.gguf", "--session-disk-limit", "14,2GB"});
    assertTrue(disk_gb.error.empty());
    assertEquals(14, disk_gb.options.config.session_disk_max_age_days);
    assertEquals(static_cast<uint64_t>(2) * 1024 * 1024 * 1024, disk_gb.options.config.session_disk_max_bytes);

    const ServerArgParse bad_unit = parse_server_args({"-m", "model.gguf", "--session-memory-mb", "1TB"});
    assertTrue(bad_unit.error.find("invalid value for --session-memory-mb") != std::string::npos);
    return EXIT_SUCCESS;
}

/**
 * A missing file, invalid JSON, a wrong type, and an unknown key name the file.
 */
static int test_config_file_errors()
{
    const auto dir = make_temp_dir("server-args-errors");
    defer(std::filesystem::remove_all(dir));
    const auto missing = config_path(dir, "missing.json");
    const ServerArgParse absent = parse_server_args({"-m", "model.gguf", "--config-file", missing});
    assertTrue(absent.error.find("cannot read config file: " + missing) != std::string::npos);
    assertTrue(!absent.usage);

    const auto broken = config_path(dir, "broken.json");
    write_test_file(broken, "{ \"m\": ");
    const ServerArgParse invalid = parse_server_args({"--config-file", broken});
    assertTrue(invalid.error.find("config file " + broken) != std::string::npos);

    const auto array = config_path(dir, "array.json");
    write_test_file(array, "[\"-m\", \"model.gguf\"]");
    const ServerArgParse not_object = parse_server_args({"--config-file", array});
    assertTrue(not_object.error.find(array + ": expected a JSON object") != std::string::npos);

    const auto nested = config_path(dir, "nested.json");
    write_test_file(nested, R"({"model": "model.gguf", "config-file": "other.json"})");
    const ServerArgParse inside = parse_server_args({"--config-file", nested});
    assertTrue(inside.error.find("config-file cannot be set inside a config file") != std::string::npos);
    assertTrue(inside.error.find(nested) != std::string::npos);

    const auto unknown = config_path(dir, "unknown.json");
    write_test_file(unknown, R"({"not-a-flag": 1})");
    const ServerArgParse bad_flag = parse_server_args({"--config-file", unknown});
    assertTrue(bad_flag.usage);
    assertTrue(bad_flag.error.find("unknown argument: not-a-flag") != std::string::npos);
    assertTrue(bad_flag.error.find(unknown) != std::string::npos);

    const auto short_key = config_path(dir, "short.json");
    write_test_file(short_key, R"({"m": "model.gguf"})");
    const ServerArgParse old_key = parse_server_args({"--config-file", short_key});
    assertTrue(old_key.error.find("unknown argument: m") != std::string::npos);

    const auto typed = config_path(dir, "typed.json");
    write_test_file(typed, R"({"model": "model.gguf", "context": "32768"})");
    const ServerArgParse bad_type = parse_server_args({"--config-file", typed});
    assertTrue(bad_type.error.find("context must be an integer") != std::string::npos);
    assertTrue(!bad_type.usage);

    const auto flag = config_path(dir, "flag.json");
    write_test_file(flag, R"({"model": "model.gguf", "reasoning": 1})");
    const ServerArgParse bad_flag_type = parse_server_args({"--config-file", flag});
    assertTrue(bad_flag_type.error.find("reasoning must be a boolean") != std::string::npos);

    const auto bad_limit = config_path(dir, "limit.json");
    write_test_file(bad_limit, R"({"model": "model.gguf", "session-disk-limit": 7})");
    const ServerArgParse limit = parse_server_args({"--config-file", bad_limit});
    assertTrue(limit.error.find("session-disk-limit must be DAYS,SIZE") != std::string::npos);

    const auto sized = config_path(dir, "sized.json");
    write_test_file(sized, R"({"model": "model.gguf", "session-memory-mb": "1GB", "session-disk-limit": "7,512MB"})");
    const ServerArgParse from_sized = parse_server_args({"--config-file", sized});
    assertTrue(from_sized.error.empty());
    assertEquals(static_cast<uint64_t>(1024) * 1024 * 1024, from_sized.options.config.session_memory_bytes);
    assertEquals(7, from_sized.options.config.session_disk_max_age_days);
    assertEquals(static_cast<uint64_t>(512) * 1024 * 1024, from_sized.options.config.session_disk_max_bytes);

    const auto old_keys = config_path(dir, "retired.json");
    write_test_file(old_keys, R"({"model": "model.gguf", "kv-sessions": 2, "session-cache-size": "8G"})");
    const ServerArgParse retired = parse_server_args({"--config-file", old_keys});
    assertTrue(retired.usage);
    assertTrue(retired.error.find("unknown argument: kv-sessions") != std::string::npos);
    return EXIT_SUCCESS;
}

/**
 * Run all server argument tests
 */
int test_server_options()
{
    RUN_TEST(test_config_file_sets_every_argument);
    RUN_TEST(test_server_args_keep_defaults);
    RUN_TEST(test_command_line_overrides_config_file);
    RUN_TEST(test_config_file_strings_and_flag_like_values);
    RUN_TEST(test_server_help_and_unknown_arguments);
    RUN_TEST(test_config_file_errors);
    return EXIT_SUCCESS;
}
