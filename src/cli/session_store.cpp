#include "cli/session_store.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

std::filesystem::path SessionStore::file() const
{
    const char *home = std::getenv("HOME");
    std::filesystem::path dir = home != nullptr ? std::filesystem::path(home) / ".agents" : std::filesystem::path(".agents");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "last-session";
}

void SessionStore::remember(const AgentConfig &config, SessionID id, const std::filesystem::path &cwd) const
{
    std::ofstream out(file());
    if (!out)
    {
        return;
    }
    out << config.host << '\n' << config.port << '\n' << cwd.string() << '\n' << id << '\n';
}

std::optional<SessionID> SessionStore::recall(const AgentConfig &config, const std::filesystem::path &cwd) const
{
    std::ifstream in(file());
    if (!in)
    {
        return std::nullopt;
    }
    std::string host;
    std::string port_text;
    std::string saved_cwd;
    std::string id_text;
    if (!std::getline(in, host) || !std::getline(in, port_text) || !std::getline(in, saved_cwd) ||
        !std::getline(in, id_text))
    {
        return std::nullopt;
    }
    try
    {
        if (host != config.host || std::stoi(port_text) != config.port || saved_cwd != cwd.string())
        {
            return std::nullopt;
        }
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
    if (id_text.empty() || id_text.find_first_not_of("0123456789") != std::string::npos)
    {
        return std::nullopt;
    }
    return static_cast<SessionID>(std::stoull(id_text));
}
