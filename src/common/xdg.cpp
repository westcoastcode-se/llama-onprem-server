#include "common/xdg.hpp"

#include <cstdlib>
#include <string>

namespace
{

constexpr std::string_view kFallbackSessionDir = "/tmp/callisto/sessions";

const char *xdg_env(XdgBase kind)
{
    switch (kind)
    {
    case XdgBase::Config:
        return "XDG_CONFIG_HOME";
    case XdgBase::Data:
        return "XDG_DATA_HOME";
    case XdgBase::State:
        return "XDG_STATE_HOME";
    case XdgBase::Cache:
        return "XDG_CACHE_HOME";
    }
    return "";
}

std::filesystem::path home_default(XdgBase kind, const std::filesystem::path &home)
{
    switch (kind)
    {
    case XdgBase::Config:
        return home / ".config";
    case XdgBase::Data:
        return home / ".local" / "share";
    case XdgBase::State:
        return home / ".local" / "state";
    case XdgBase::Cache:
        return home / ".cache";
    }
    return {};
}

} // namespace

std::filesystem::path xdg_base(const XdgBase kind)
{
    if (const char *env = xdg_env(kind); env[0] != '\0')
    {
        if (const char *value = std::getenv(env); value != nullptr && value[0] != '\0')
        {
            return std::filesystem::path(value);
        }
    }
    const char *home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0')
    {
        return {};
    }
    return home_default(kind, std::filesystem::path(home));
}

std::filesystem::path callisto_user_dir(const XdgBase kind)
{
    const std::filesystem::path base = xdg_base(kind);
    if (base.empty())
    {
        return {};
    }
    return base / "callisto";
}

SessionDirs default_session_dirs(const std::string_view session_dir_override)
{
    if (!session_dir_override.empty())
    {
        const std::filesystem::path dir{std::string(session_dir_override)};
        return SessionDirs{dir, dir};
    }
    SessionDirs dirs;
    dirs.conversations = callisto_user_dir(XdgBase::State);
    dirs.kv = callisto_user_dir(XdgBase::Cache);
    if (dirs.conversations.empty())
    {
        dirs.conversations = kFallbackSessionDir;
    }
    else
    {
        dirs.conversations /= "sessions";
    }
    if (dirs.kv.empty())
    {
        dirs.kv = kFallbackSessionDir;
    }
    else
    {
        dirs.kv /= "sessions";
    }
    return dirs;
}
