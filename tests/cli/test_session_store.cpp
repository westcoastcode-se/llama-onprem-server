//
// File containing tests for the last-session file
//

#include "cli/session_store.hpp"
#include "common/defer.hpp"
#include "../tests.hpp"

#include <cstdlib>
#include <fstream>
#include <vector>

namespace
{

class HomeGuard
{
    struct Saved
    {
        std::string name;
        std::string value;
        bool had = false;
    };
    std::vector<Saved> saved_;

    void keep(const char *name)
    {
        Saved item;
        item.name = name;
        if (const char *old = std::getenv(name))
        {
            item.had = true;
            item.value = old;
        }
        saved_.push_back(std::move(item));
    }

  public:
    explicit HomeGuard(const std::filesystem::path &home)
    {
        keep("HOME");
        keep("XDG_CONFIG_HOME");
        keep("XDG_DATA_HOME");
        keep("XDG_STATE_HOME");
        keep("XDG_CACHE_HOME");
        setenv("HOME", home.c_str(), 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        unsetenv("XDG_STATE_HOME");
        unsetenv("XDG_CACHE_HOME");
    }

    ~HomeGuard()
    {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it)
        {
            if (it->had)
            {
                setenv(it->name.c_str(), it->value.c_str(), 1);
            }
            else
            {
                unsetenv(it->name.c_str());
            }
        }
    }
};

std::filesystem::path state_dir(const std::filesystem::path &home)
{
    return home / ".local" / "state" / "callisto";
}

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
 * Every id remembered on this computer can be resumed. Another host cannot use them.
 * Remembering the same id twice keeps a single registry line.
 */
static int test_session_store_owns_created_ids() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";

    AgentConfig config;
    SessionStore store;
    assertTrue(!store.owns(config, 42));

    store.remember(config, 42, cwd);
    store.remember(config, 42, cwd);
    store.remember(config, 99, home / "other");

    assertTrue(store.owns(config, 42));
    assertTrue(store.owns(config, 99));
    assertTrue(!store.owns(config, 7));
    assertTrue(!store.owns(config, 0));

    AgentConfig other_host = config;
    other_host.host = "10.0.0.1";
    assertTrue(!store.owns(other_host, 42));

    AgentConfig other_port = config;
    other_port.port = 9;
    assertTrue(!store.owns(other_port, 99));

    const auto latest = store.recall(config, home / "other");
    assertTrue(latest.has_value());
    assertEquals(static_cast<SessionID>(99), *latest);
    assertTrue(!store.recall(config, cwd).has_value());

    std::ifstream registry(state_dir(home) / "known-sessions");
    int lines = 0;
    std::string line;
    while (std::getline(registry, line))
    {
        if (!line.empty())
        {
            ++lines;
        }
    }
    assertEquals(2, lines);
    assertTrue(std::filesystem::exists(state_dir(home) / "last-session"));
    assertTrue(!std::filesystem::exists(home / ".agents" / "sessions"));
    assertTrue(!std::filesystem::exists(home / ".agents" / "last-session"));
    return EXIT_SUCCESS;
}

/**
 * Recording an id makes it resumable without replacing the directory's last session.
 */
static int test_session_store_record_leaves_last_session() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";

    AgentConfig config;
    SessionStore store;
    store.remember(config, 7, cwd);
    store.record(config, 42);

    assertTrue(store.owns(config, 42));
    const auto saved = store.recall(config, cwd);
    assertTrue(saved.has_value());
    assertEquals(static_cast<SessionID>(7), *saved);
    return EXIT_SUCCESS;
}

/**
 * A last-session file counts as created here before it is copied into the registry.
 */
static int test_session_store_owns_last_session() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";
    write_test_file(state_dir(home) / "last-session", "127.0.0.1\n8080\n" + cwd.string() + "\n42\n");

    AgentConfig config;
    SessionStore store;
    assertTrue(store.owns(config, 42));
    assertTrue(!store.owns(config, 7));
    const auto id = store.recall(config, cwd);
    assertTrue(id.has_value());
    assertEquals(static_cast<SessionID>(42), *id);
    assertTrue(!std::filesystem::exists(home / ".agents" / "sessions"));
    return EXIT_SUCCESS;
}

/**
 * A broken registry line is skipped. A well-formed later line still counts.
 */
static int test_session_store_skips_bad_registry_line() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    write_test_file(state_dir(home) / "known-sessions", "nope\n42\t127.0.0.1\t8080\n");

    AgentConfig config;
    SessionStore store;
    assertTrue(store.owns(config, 42));
    assertTrue(!store.owns(config, 1));
    return EXIT_SUCCESS;
}

/**
 * Files left under ~/.agents are not a session list.
 */
static int test_session_store_ignores_agents_files() {
    const auto home = make_temp_dir("session");
    defer(std::filesystem::remove_all(home));
    const HomeGuard guard(home);
    const auto cwd = home / "work";
    write_test_file(home / ".agents" / "last-session", "127.0.0.1\n8080\n" + cwd.string() + "\n42\n");
    write_test_file(home / ".agents" / "sessions", "42\t127.0.0.1\t8080\n");

    AgentConfig config;
    SessionStore store;
    assertTrue(!store.owns(config, 42));
    assertTrue(!store.recall(config, cwd).has_value());
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
    write_test_file(state_dir(home) / "last-session", "127.0.0.1\nabc\n" + cwd.string() + "\n1\n");

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
    RUN_TEST(test_session_store_owns_created_ids);
    RUN_TEST(test_session_store_record_leaves_last_session);
    RUN_TEST(test_session_store_owns_last_session);
    RUN_TEST(test_session_store_ignores_agents_files);
    RUN_TEST(test_session_store_skips_bad_registry_line);
    RUN_TEST(test_session_store_corrupt);
    return EXIT_SUCCESS;
}
