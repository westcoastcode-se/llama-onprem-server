#include "common/xdg.hpp"
#include "../tests.hpp"

#include <cstdlib>
#include <vector>

namespace
{

class EnvGuard
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
        unsetenv(name);
    }

  public:
    EnvGuard()
    {
        keep("HOME");
        keep("XDG_CONFIG_HOME");
        keep("XDG_DATA_HOME");
        keep("XDG_STATE_HOME");
        keep("XDG_CACHE_HOME");
    }

    ~EnvGuard()
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

/**
 * With HOME set and the XDG variables unset, each base is the usual directory under home.
 */
static int test_xdg_defaults_under_home()
{
    const EnvGuard guard;
    const auto home = std::filesystem::path("/tmp/callisto-xdg-home");
    setenv("HOME", home.c_str(), 1);

    assertEquals(home / ".config", xdg_base(XdgBase::Config));
    assertEquals(home / ".local" / "share", xdg_base(XdgBase::Data));
    assertEquals(home / ".local" / "state", xdg_base(XdgBase::State));
    assertEquals(home / ".cache", xdg_base(XdgBase::Cache));
    assertEquals(home / ".config" / "callisto", callisto_user_dir(XdgBase::Config));
    assertEquals(home / ".local" / "share" / "callisto", callisto_user_dir(XdgBase::Data));
    assertEquals(home / ".local" / "state" / "callisto", callisto_user_dir(XdgBase::State));
    assertEquals(home / ".cache" / "callisto", callisto_user_dir(XdgBase::Cache));

    const SessionDirs dirs = default_session_dirs("");
    assertEquals(home / ".local" / "state" / "callisto" / "sessions", dirs.conversations);
    assertEquals(home / ".cache" / "callisto" / "sessions", dirs.kv);
    return EXIT_SUCCESS;
}

/**
 * A set XDG variable wins over HOME. An empty one is unset and falls back to HOME.
 */
static int test_xdg_env_wins_and_empty_falls_back()
{
    const EnvGuard guard;
    const auto home = std::filesystem::path("/tmp/callisto-xdg-home");
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", "/tmp/xdg-config", 1);
    setenv("XDG_STATE_HOME", "", 1);

    assertEquals(std::filesystem::path("/tmp/xdg-config"), xdg_base(XdgBase::Config));
    assertEquals(std::filesystem::path("/tmp/xdg-config") / "callisto", callisto_user_dir(XdgBase::Config));
    assertEquals(home / ".local" / "state", xdg_base(XdgBase::State));
    return EXIT_SUCCESS;
}

/**
 * An explicit session directory stores the conversation and the KV cache together.
 */
static int test_xdg_session_override_is_one_directory()
{
    const EnvGuard guard;
    const SessionDirs dirs = default_session_dirs("/var/sessions");
    assertEquals(std::filesystem::path("/var/sessions"), dirs.conversations);
    assertEquals(dirs.conversations, dirs.kv);
    return EXIT_SUCCESS;
}

/**
 * With neither HOME nor an XDG variable, bases are empty and sessions use the temporary fallback.
 */
static int test_xdg_without_home_uses_fallback()
{
    const EnvGuard guard;
    assertTrue(xdg_base(XdgBase::Config).empty());
    assertTrue(xdg_base(XdgBase::Data).empty());
    assertTrue(xdg_base(XdgBase::State).empty());
    assertTrue(xdg_base(XdgBase::Cache).empty());
    assertTrue(callisto_user_dir(XdgBase::State).empty());

    const SessionDirs dirs = default_session_dirs("");
    assertEquals(std::filesystem::path("/tmp/callisto/sessions"), dirs.conversations);
    assertEquals(dirs.conversations, dirs.kv);
    return EXIT_SUCCESS;
}

} // namespace

int test_xdg()
{
    RUN_TEST(test_xdg_defaults_under_home);
    RUN_TEST(test_xdg_env_wins_and_empty_falls_back);
    RUN_TEST(test_xdg_session_override_is_one_directory);
    RUN_TEST(test_xdg_without_home_uses_fallback);
    return EXIT_SUCCESS;
}
