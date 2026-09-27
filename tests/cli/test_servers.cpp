#include "cli/servers.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <cstdlib>
#include <fstream>

namespace
{

class HomeGuard
{
    std::string previous_;
    bool had_ = false;

  public:
    explicit HomeGuard(const std::filesystem::path &home)
    {
        if (const char *old = std::getenv("HOME"))
        {
            had_ = true;
            previous_ = old;
        }
        setenv("HOME", home.c_str(), 1);
    }

    ~HomeGuard()
    {
        if (had_)
        {
            setenv("HOME", previous_.c_str(), 1);
        }
        else
        {
            unsetenv("HOME");
        }
    }
};

int test_parse_server_host_port()
{
    const ServerTarget server = parse_server_spec("127.0.0.1:9090");
    assertEquals(std::string("127.0.0.1"), server.host);
    assertEquals(9090, server.port);
    assertEquals(std::string("127.0.0.1:9090"), server.label());
    assertEquals(std::string("http://127.0.0.1:9090"), server.url());
    return 0;
}

int test_parse_server_name_and_url()
{
    const ServerTarget server = parse_server_spec(" qwen = http://localhost:8081/ ");
    assertEquals(std::string("qwen"), server.name);
    assertEquals(std::string("localhost"), server.host);
    assertEquals(8081, server.port);
    assertEquals(std::string("qwen"), server.label());
    return 0;
}

int test_parse_server_list_commas()
{
    const std::vector<ServerTarget> servers = parse_server_list({"qwen=127.0.0.1:8080, devstral=10.0.0.2:8081"});
    assertEquals(static_cast<std::size_t>(2), servers.size());
    assertEquals(std::string("qwen"), servers[0].name);
    assertEquals(8080, servers[0].port);
    assertEquals(std::string("devstral"), servers[1].name);
    assertEquals(std::string("10.0.0.2"), servers[1].host);
    assertEquals(8081, servers[1].port);
    return 0;
}

int test_parse_server_rejects_bad_port()
{
    try
    {
        const ServerTarget ignored = parse_server_spec("localhost:70000");
        (void)ignored;
    }
    catch (const std::runtime_error &)
    {
        return 0;
    }
    return EXIT_FAILURE;
}

int test_servers_from_json()
{
    const char *text = R"({
        "servers": [
            {"model": "qwen", "host": "127.0.0.1", "port": 8080},
            {"name": "bonsai", "url": "http://127.0.0.1:8082"},
            "plain=10.1.1.1:9000"
        ]
    })";
    const std::vector<ServerTarget> servers = servers_from_json(text);
    assertEquals(static_cast<std::size_t>(3), servers.size());
    assertEquals(std::string("qwen"), servers[0].label());
    assertEquals(8080, servers[0].port);
    assertEquals(std::string("bonsai"), servers[1].name);
    assertEquals(8082, servers[1].port);
    assertEquals(std::string("plain"), servers[2].name);
    assertEquals(std::string("10.1.1.1"), servers[2].host);
    assertEquals(9000, servers[2].port);
    return 0;
}

int test_servers_json_model_wins_over_name()
{
    const std::vector<ServerTarget> servers =
        servers_from_json(R"([{"name": "ignored", "model": "qwen", "host": "localhost"}])");
    assertEquals(static_cast<std::size_t>(1), servers.size());
    assertEquals(std::string("qwen"), servers[0].name);
    assertEquals(8080, servers[0].port);
    return 0;
}

int test_load_servers_file_missing_is_empty()
{
    const auto home = make_temp_dir("servers");
    defer(std::filesystem::remove_all(home));
    const std::vector<ServerTarget> servers = load_servers_file(home / "missing.json");
    assertTrue(servers.empty());
    return 0;
}

int test_load_servers_file_round_trip()
{
    const auto home = make_temp_dir("servers");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto path = servers_config_path();
    assertEquals((home / ".config" / "callisto" / "servers.json").string(), path.string());
    write_test_file(path, R"([{"model": "qwen", "host": "127.0.0.1", "port": 8080}])");
    const std::vector<ServerTarget> servers = load_servers_file(path);
    assertEquals(static_cast<std::size_t>(1), servers.size());
    assertEquals(std::string("qwen"), servers[0].label());
    return 0;
}

int test_first_reachable_skips_down_servers()
{
    const auto none = first_reachable(3, [](std::size_t) { return false; });
    assertTrue(!none.has_value());
    const auto second = first_reachable(3, [](std::size_t index) { return index == 1; });
    assertTrue(second.has_value());
    assertEquals(static_cast<std::size_t>(1), *second);
    const auto first = first_reachable(2, [](std::size_t index) { return index == 0; });
    assertTrue(first.has_value());
    assertEquals(static_cast<std::size_t>(0), *first);
    return 0;
}

} // namespace

int test_servers()
{
    RUN_TEST(test_parse_server_host_port);
    RUN_TEST(test_parse_server_name_and_url);
    RUN_TEST(test_parse_server_list_commas);
    RUN_TEST(test_parse_server_rejects_bad_port);
    RUN_TEST(test_servers_from_json);
    RUN_TEST(test_servers_json_model_wins_over_name);
    RUN_TEST(test_load_servers_file_missing_is_empty);
    RUN_TEST(test_load_servers_file_round_trip);
    RUN_TEST(test_first_reachable_skips_down_servers);
    return 0;
}
