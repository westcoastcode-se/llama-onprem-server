#include "cli/servers.hpp"
#include "../tests.hpp"

#include <cstdlib>

namespace
{

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
    const auto both = first_reachable(2, [](std::size_t) { return true; });
    assertTrue(both.has_value());
    assertEquals(static_cast<std::size_t>(0), *both);
    return 0;
}

} // namespace

int test_servers()
{
    RUN_TEST(test_parse_server_host_port);
    RUN_TEST(test_parse_server_name_and_url);
    RUN_TEST(test_parse_server_list_commas);
    RUN_TEST(test_parse_server_rejects_bad_port);
    RUN_TEST(test_first_reachable_skips_down_servers);
    return 0;
}
