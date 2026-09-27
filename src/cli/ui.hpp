#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

inline std::atomic<bool> g_agent_interrupt{false};

enum class Ask
{
    Once,
    AlwaysTool,
    Full,
    Deny,
    Closed
};

// Where the agent draws itself. exec uses the console. The interactive session uses FTXUI.
struct AgentUi
{
    virtual ~AgentUi() = default;
    virtual void set_status(std::string status) = 0;
    // used and size are token counts. size 0 means the window length is unknown.
    virtual void set_context(int used, int size) = 0;
    virtual void note(std::string text) = 0;
    virtual void begin(std::string kind) = 0;
    virtual void append(std::string text) = 0;
    virtual void end() = 0;
    // The newest block of this kind. The console session ignores these.
    virtual void expand(std::string_view) {}
    virtual void collapse(std::string_view) {}
    virtual void caption(std::string) {}
    virtual Ask ask(std::string title, std::string body) = 0;
    virtual std::optional<std::string> question(std::string prompt, std::vector<std::string> answers) = 0;
    // Row index. nullopt when the user cancels. selected is the highlighted row.
    virtual std::optional<std::size_t> choose(std::string prompt, std::vector<std::string> choices, std::size_t selected) = 0;
    // Empty when the user leaves the session.
    virtual std::string read_line() = 0;
};
