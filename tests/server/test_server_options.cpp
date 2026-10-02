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
        "m": "/tmp/my model.gguf",
        "c": 32768,
        "b": 1024,
        "ngl": 40,
        "t": 0.5,
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
        "kv-sessions": 3,
        "session-dir": "/tmp/session dir",
        "session-cache-size": "8G",
        "host": "0.0.0.0",
        "port": 9090
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
    assertEquals(3, config.kv_sessions);
    assertEquals(std::string("/tmp/session dir"), config.session_dir);
    assertEquals(static_cast<uint64_t>(8) * 1024 * 1024 * 1024, config.session_cache_bytes);
    assertEquals(std::string("0.0.0.0"), parsed.options.host);
    assertEquals(9090, parsed.options.port);
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
    assertEquals(2, config.kv_sessions);
    assertEquals(std::string(kDefaultSessionDir), config.session_dir);
    assertEquals(static_cast<uint64_t>(0), config.session_cache_bytes);
    assertEquals(std::string("127.0.0.1"), parsed.options.host);
    assertEquals(8080, parsed.options.port);
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
        "m": "from-file.gguf",
        "c": 111,
        "host": "1.1.1.1",
        "port": 2222,
        "reasoning": false,
        "session-cache-size": "1K"
    })");
    write_test_file(second, R"({
        "c": 222,
        "p": 3333,
        "session-cache-size": 2097152
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
        "--session-cache-size",
        "0",
    });
    assertTrue(parsed.error.empty());
    assertEquals(std::string("from-cli.gguf"), parsed.options.config.model_path);
    assertEquals(1, parsed.options.config.n_ctx);
    assertEquals(std::string("1.1.1.1"), parsed.options.host);
    assertEquals(9, parsed.options.port);
    assertTrue(parsed.options.config.reasoning);
    assertEquals(-1, parsed.options.config.max_tokens);
    assertEquals(static_cast<uint64_t>(0), parsed.options.config.session_cache_bytes);

    const ServerArgParse from_files = parse_server_args({"-m", "cli.gguf", "--config-file", first, "--config-file", second});
    assertEquals(222, from_files.options.config.n_ctx);
    assertEquals(3333, from_files.options.port);
    assertEquals(static_cast<uint64_t>(2) * 1024 * 1024, from_files.options.config.session_cache_bytes);
    assertTrue(!from_files.options.config.reasoning);
    assertEquals(std::string("1.1.1.1"), from_files.options.host);
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
        "m": "model.gguf",
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

    const ServerArgParse bad_size = parse_server_args({"-m", "model.gguf", "--session-cache-size", "12xigs"});
    assertEquals(std::string("invalid --session-cache-size"), bad_size.error);
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
    write_test_file(nested, R"({"m": "model.gguf", "config-file": "other.json"})");
    const ServerArgParse inside = parse_server_args({"--config-file", nested});
    assertTrue(inside.error.find("config-file cannot be set inside a config file") != std::string::npos);
    assertTrue(inside.error.find(nested) != std::string::npos);

    const auto unknown = config_path(dir, "unknown.json");
    write_test_file(unknown, R"({"not-a-flag": 1})");
    const ServerArgParse bad_flag = parse_server_args({"--config-file", unknown});
    assertTrue(bad_flag.usage);
    assertTrue(bad_flag.error.find("unknown argument: not-a-flag") != std::string::npos);
    assertTrue(bad_flag.error.find(unknown) != std::string::npos);

    const auto typed = config_path(dir, "typed.json");
    write_test_file(typed, R"({"m": "model.gguf", "c": "32768"})");
    const ServerArgParse bad_type = parse_server_args({"--config-file", typed});
    assertTrue(bad_type.error.find("c must be an integer") != std::string::npos);
    assertTrue(!bad_type.usage);

    const auto flag = config_path(dir, "flag.json");
    write_test_file(flag, R"({"m": "model.gguf", "reasoning": 1})");
    const ServerArgParse bad_flag_type = parse_server_args({"--config-file", flag});
    assertTrue(bad_flag_type.error.find("reasoning must be a boolean") != std::string::npos);

    const auto bad_size = config_path(dir, "size.json");
    write_test_file(bad_size, R"({"m": "model.gguf", "session-cache-size": "12xigs"})");
    const ServerArgParse size = parse_server_args({"--config-file", bad_size});
    assertTrue(size.error.find("invalid session-cache-size") != std::string::npos);
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
