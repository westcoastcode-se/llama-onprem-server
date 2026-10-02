#include "cli/session_store.hpp"

#include "common/xdg.hpp"

#include <fstream>
#include <string>
#include <utility>

namespace
{

struct SavedSession
{
    std::string host;
    int port = 0;
    std::string cwd;
    SessionID id = 0;
};

bool parse_port(const std::string &text, int &port)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    try
    {
        std::size_t used = 0;
        const int value = std::stoi(text, &used, 10);
        if (used != text.size() || value <= 0)
        {
            return false;
        }
        port = value;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool parse_id(const std::string &text, SessionID &id)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    try
    {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(text, &used, 10);
        if (used != text.size() || value == 0)
        {
            return false;
        }
        id = static_cast<SessionID>(value);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::optional<SavedSession> read_saved(const std::filesystem::path &path)
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    SavedSession saved;
    std::string port_text;
    std::string id_text;
    if (!std::getline(in, saved.host) || !std::getline(in, port_text) || !std::getline(in, saved.cwd) ||
        !std::getline(in, id_text))
    {
        return std::nullopt;
    }
    if (!parse_port(port_text, saved.port) || !parse_id(id_text, saved.id))
    {
        return std::nullopt;
    }
    return saved;
}

bool same_server(const SavedSession &saved, const AgentConfig &config)
{
    return saved.host == config.host && saved.port == config.port;
}

// One registry line is "id\\thost\\tport". A short or foreign line is skipped.
bool recorded(const std::filesystem::path &path, const AgentConfig &config, const SessionID id)
{
    std::ifstream in(path);
    if (!in)
    {
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos || line.find('\t', second + 1) != std::string::npos)
        {
            continue;
        }
        SessionID line_id = 0;
        int port = 0;
        const std::string host = line.substr(first + 1, second - first - 1);
        if (host != config.host || !parse_id(line.substr(0, first), line_id) || !parse_port(line.substr(second + 1), port))
        {
            continue;
        }
        if (line_id == id && port == config.port)
        {
            return true;
        }
    }
    return false;
}

// Empty when neither $XDG_STATE_HOME nor HOME is set.
std::filesystem::path state_dir()
{
    const std::filesystem::path dir = callisto_user_dir(XdgBase::State);
    if (dir.empty())
    {
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

std::filesystem::path SessionStore::file() const
{
    const std::filesystem::path dir = state_dir();
    if (dir.empty())
    {
        return {};
    }
    return dir / "last-session";
}

std::filesystem::path SessionStore::registry() const
{
    const std::filesystem::path dir = state_dir();
    if (dir.empty())
    {
        return {};
    }
    return dir / "known-sessions";
}

void SessionStore::record(const AgentConfig &config, const SessionID id) const
{
    if (id == 0 || config.host.find_first_of("\t\r\n") != std::string::npos)
    {
        return;
    }
    const std::filesystem::path path = registry();
    if (path.empty() || recorded(path, config, id))
    {
        return;
    }
    const std::string line = std::to_string(id) + '\t' + config.host + '\t' + std::to_string(config.port) + '\n';
    std::ofstream out(path, std::ios::app);
    if (!out)
    {
        return;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
}

void SessionStore::remember(const AgentConfig &config, const SessionID id, const std::filesystem::path &cwd) const
{
    if (id == 0 || config.host.find_first_of("\t\r\n") != std::string::npos)
    {
        return;
    }
    const std::filesystem::path path = file();
    if (path.empty())
    {
        return;
    }
    std::ofstream out(path);
    if (out)
    {
        out << config.host << '\n' << config.port << '\n' << cwd.string() << '\n' << id << '\n';
    }
    record(config, id);
}

std::optional<SessionID> SessionStore::recall(const AgentConfig &config, const std::filesystem::path &cwd) const
{
    const auto saved = read_saved(file());
    if (!saved || !same_server(*saved, config) || saved->cwd != cwd.string())
    {
        return std::nullopt;
    }
    return saved->id;
}

bool SessionStore::owns(const AgentConfig &config, const SessionID id) const
{
    if (id == 0)
    {
        return false;
    }
    if (recorded(registry(), config, id))
    {
        return true;
    }
    const auto saved = read_saved(file());
    return saved && same_server(*saved, config) && saved->id == id;
}
