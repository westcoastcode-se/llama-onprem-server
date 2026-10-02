#pragma once

#include <filesystem>
#include <string_view>

// XDG Base Directory. An empty environment variable is treated as unset.
// Config is $XDG_CONFIG_HOME or ~/.config.
// Data is $XDG_DATA_HOME or ~/.local/share. Callisto stores nothing there.
// State is $XDG_STATE_HOME or ~/.local/state.
// Cache is $XDG_CACHE_HOME or ~/.cache.
enum class XdgBase
{
    Config,
    Data,
    State,
    Cache
};

// The base directory, or empty when neither the XDG variable nor HOME is set.
[[nodiscard]] std::filesystem::path xdg_base(XdgBase kind);

// xdg_base(kind) / "callisto". Empty when the base is empty.
[[nodiscard]] std::filesystem::path callisto_user_dir(XdgBase kind);

// Parked sessions. An override stores the conversation and the KV cache together.
// Otherwise the conversation is under the state directory and the KV cache under the cache directory.
// With no home directory, both use /tmp/callisto/sessions.
struct SessionDirs
{
    std::filesystem::path conversations;
    std::filesystem::path kv;
};

[[nodiscard]] SessionDirs default_session_dirs(std::string_view session_dir_override);
