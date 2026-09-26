//
// File containing tests for the last-session file
//

#include "cli/session_store.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <cstdlib>

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

} // namespace

/**
 * A remembered id is returned for the same host, port, and directory.
 */
static int test_session_store_round_trip() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";

    AgentConfig config;
    SessionStore store;
    assertTrue(!store.recall(config, cwd).has_value());

    store.remember(config, 42, cwd);
    const auto id = store.recall(config, cwd);
    assertTrue(id.has_value());
    assertEquals(static_cast<SessionID>(42), *id);
    return EXIT_SUCCESS;
}

/**
 * A different host, port, or directory does not reuse the saved id.
 */
static int test_session_store_mismatch() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";

    AgentConfig config;
    SessionStore store;
    store.remember(config, 7, cwd);

    AgentConfig other_host = config;
    other_host.host = "10.0.0.1";
    assertTrue(!store.recall(other_host, cwd).has_value());

    AgentConfig other_port = config;
    other_port.port = 9;
    assertTrue(!store.recall(other_port, cwd).has_value());

    assertTrue(!store.recall(config, home / "elsewhere").has_value());
    return EXIT_SUCCESS;
}

/**
 * A file that is not four well-formed lines is ignored.
 */
static int test_session_store_corrupt() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";
    write_test_file(home / ".callisto" / "last-session", "127.0.0.1\nabc\n" + cwd.string() + "\n1\n");

    AgentConfig config;
    SessionStore store;
    assertTrue(!store.recall(config, cwd).has_value());
    return EXIT_SUCCESS;
}

/**
 * Run all session store tests
 */
int test_session_store() {
    RUN_TEST(test_session_store_round_trip);
    RUN_TEST(test_session_store_mismatch);
    RUN_TEST(test_session_store_corrupt);
    return EXIT_SUCCESS;
}
