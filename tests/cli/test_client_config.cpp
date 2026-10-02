#include "cli/client_config.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <CLI11.hpp>

#include <string>
#include <vector>

namespace
{

std::string config_path(const std::filesystem::path &dir, const char *name)
{
    return (dir / name).string();
}

} // namespace

/**
 * A config file sets the client settings. A later file overrides the keys it sets.
 */
static int test_client_config_file_sets_and_layers()
{
    const auto dir = make_temp_dir("client-config");
    defer(std::filesystem::remove_all(dir));
    const auto first = config_path(dir, "first.json");
    const auto second = config_path(dir, "second.json");
    write_test_file(first, R"({
        "host": "10.0.0.1",
        "port": 9090,
        "json": true,
        "verbose": true,
        "server": ["qwen=10.0.0.1:9090", "other=10.0.0.2:9091"],
        "approval": "auto",
        "resume": true,
        "session": "42",
        "show-think": false,
        "debug": true,
        "questions": false,
        "compress-tools": false,
        "theme": "nord"
    })");
    write_test_file(second, R"({
        "port": 9091,
        "server": "only=127.0.0.1:1",
        "approval": "suggest"
    })");

    ClientConfig config;
    assertEquals(std::string(), load_client_config_file(first, config));
    assertEquals(std::string("10.0.0.1"), config.host);
    assertEquals(9090, config.port);
    assertTrue(config.json);
    assertTrue(config.verbose);
    assertEquals(static_cast<std::size_t>(2), config.servers.size());
    assertEquals(std::string("qwen=10.0.0.1:9090"), config.servers[0]);
    assertEquals(std::string("other=10.0.0.2:9091"), config.servers[1]);
    assertEquals(std::string("auto"), config.approval);
    assertTrue(config.resume);
    assertEquals(std::string("42"), config.session);
    assertTrue(!config.show_think);
    assertTrue(config.debug);
    assertTrue(!config.questions);
    assertTrue(!config.compress_tools);
    assertEquals(std::string("nord"), config.theme);

    assertEquals(std::string(), load_client_config_file(second, config));
    assertEquals(std::string("10.0.0.1"), config.host);
    assertEquals(9091, config.port);
    assertEquals(static_cast<std::size_t>(1), config.servers.size());
    assertEquals(std::string("only=127.0.0.1:1"), config.servers[0]);
    assertEquals(std::string("suggest"), config.approval);
    assertTrue(config.resume);
    return EXIT_SUCCESS;
}

/**
 * --config-file pairs leave the argv. Help is noticed and still keeps its flag.
 */
static int test_split_client_args()
{
    const ClientArgSplit split = split_client_args(
        {"--host", "1.2.3.4", "--config-file", "a.json", "exec", "--config-file", "b.json", "hello"});
    assertTrue(split.error.empty());
    assertTrue(!split.help);
    assertEquals(static_cast<std::size_t>(2), split.config_files.size());
    assertEquals(std::string("a.json"), split.config_files[0]);
    assertEquals(std::string("b.json"), split.config_files[1]);
    assertEquals(static_cast<std::size_t>(4), split.args.size());
    assertEquals(std::string("--host"), split.args[0]);
    assertEquals(std::string("1.2.3.4"), split.args[1]);
    assertEquals(std::string("exec"), split.args[2]);
    assertEquals(std::string("hello"), split.args[3]);

    const ClientArgSplit help = split_client_args({"--config-file", "a.json", "--help"});
    assertTrue(help.help);
    assertTrue(help.error.empty());
    assertEquals(static_cast<std::size_t>(1), help.config_files.size());
    assertEquals(static_cast<std::size_t>(1), help.args.size());
    assertEquals(std::string("--help"), help.args[0]);

    const ClientArgSplit help_path = split_client_args({"--config-file", "--help"});
    assertTrue(help_path.help);
    assertTrue(help_path.error.empty());
    assertTrue(help_path.config_files.empty());
    assertEquals(std::string("--help"), help_path.args[0]);

    const ClientArgSplit missing = split_client_args({"--config-file"});
    assertEquals(std::string("missing value for --config-file"), missing.error);
    return EXIT_SUCCESS;
}

/**
 * A missing file, bad JSON, a bad type, and an unknown key name the file.
 */
static int test_client_config_file_errors()
{
    const auto dir = make_temp_dir("client-config-errors");
    defer(std::filesystem::remove_all(dir));
    ClientConfig config;

    const auto missing = config_path(dir, "missing.json");
    const std::string absent = load_client_config_file(missing, config);
    assertTrue(absent.find("cannot read config file: " + missing) != std::string::npos);

    const auto broken = config_path(dir, "broken.json");
    write_test_file(broken, "{ \"host\": ");
    const std::string invalid = load_client_config_file(broken, config);
    assertTrue(invalid.find("config file " + broken) != std::string::npos);

    const auto array = config_path(dir, "array.json");
    write_test_file(array, "[\"host\"]");
    const std::string not_object = load_client_config_file(array, config);
    assertTrue(not_object.find(array + ": expected a JSON object") != std::string::npos);

    const auto nested = config_path(dir, "nested.json");
    write_test_file(nested, R"({"config-file": "other.json"})");
    const std::string inside = load_client_config_file(nested, config);
    assertTrue(inside.find("config-file cannot be set inside a config file") != std::string::npos);

    const auto unknown = config_path(dir, "unknown.json");
    write_test_file(unknown, R"({"m": "model"})");
    const std::string bad_key = load_client_config_file(unknown, config);
    assertTrue(bad_key.find("unknown argument: m") != std::string::npos);

    const auto typed = config_path(dir, "typed.json");
    write_test_file(typed, R"({"port": "9090"})");
    const std::string bad_type = load_client_config_file(typed, config);
    assertTrue(bad_type.find("port must be an integer") != std::string::npos);

    const auto range = config_path(dir, "range.json");
    write_test_file(range, R"({"port": 70000})");
    const std::string bad_port = load_client_config_file(range, config);
    assertTrue(bad_port.find("port must be an integer from 1 to 65535") != std::string::npos);

    const auto approval = config_path(dir, "approval.json");
    write_test_file(approval, R"({"approval": "yolo"})");
    const std::string bad_approval = load_client_config_file(approval, config);
    assertTrue(bad_approval.find("approval must be read-only, auto, or full") != std::string::npos);

    const auto servers = config_path(dir, "servers.json");
    write_test_file(servers, R"({"server": [1]})");
    const std::string bad_server = load_client_config_file(servers, config);
    assertTrue(bad_server.find("server must be a string or an array of strings") != std::string::npos);
    return EXIT_SUCCESS;
}

/**
 * The file is applied after CLI11 records its defaults, and a flag replaces the file.
 */
static int test_client_flags_override_config_file()
{
    const auto dir = make_temp_dir("client-config-override");
    defer(std::filesystem::remove_all(dir));
    const auto path = config_path(dir, "client.json");
    write_test_file(path, R"({
        "host": "10.0.0.1",
        "approval": "auto",
        "resume": true,
        "server": ["qwen=10.0.0.1:9"]
    })");

    ClientConfig config;
    CLI::App app{"test"};
    app.add_option("--host", config.host, "Server host")->default_val("127.0.0.1");
    app.add_option("--approval", config.approval, "approval")->default_val("read-only");
    app.add_flag("--resume", config.resume, "resume");
    app.add_option("--server", config.servers, "servers");

    assertEquals(std::string(), load_client_config_file(path, config));
    app.parse("--approval full --server cli=127.0.0.1:1", false);
    assertEquals(std::string("10.0.0.1"), config.host);
    assertEquals(std::string("full"), config.approval);
    assertTrue(config.resume);
    assertEquals(static_cast<std::size_t>(1), config.servers.size());
    assertEquals(std::string("cli=127.0.0.1:1"), config.servers[0]);
    return EXIT_SUCCESS;
}

int test_client_config()
{
    RUN_TEST(test_client_config_file_sets_and_layers);
    RUN_TEST(test_split_client_args);
    RUN_TEST(test_client_config_file_errors);
    RUN_TEST(test_client_flags_override_config_file);
    return EXIT_SUCCESS;
}
