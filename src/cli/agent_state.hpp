#pragma once

#include "cli/agent.hpp"
#include "cli/ui.hpp"

#include "api/models.hpp"
#include "client/rest_client.hpp"

#include <filesystem>
#include <vector>

enum class TurnStatus
{
    Idle,
    Cancelled,
    ContextFull
};

// One configured model server. The client pointer is owned by run_agent.
struct ServerSlot
{
    ServerTarget target;
    RestClient *client = nullptr;
    SessionID session = 0;
};

// Shared context for one client process. The pointers are owned by run_agent.
struct AgentState
{
    std::vector<ServerSlot> servers;
    std::size_t active = 0;
    RestClient *client = nullptr;
    SessionID session = 0;
    // True until a server answers. The UI stays open and chat is refused.
    bool awaiting_server = false;
    ApprovalMode approval = ApprovalMode::ReadOnly;
    bool show_think = true;
    bool debug = false;
    bool exec = false;
    int max_rounds = 40;
    std::vector<std::string> always_tools;
    std::filesystem::path cwd = std::filesystem::current_path();
    AgentUi *ui = nullptr;
};

[[nodiscard]] inline const char *approval_name(ApprovalMode mode)
{
    switch (mode)
    {
    case ApprovalMode::ReadOnly:
        return "read-only";
    case ApprovalMode::Auto:
        return "auto";
    case ApprovalMode::Full:
        return "full";
    }
    return "read-only";
}

inline void show_context(AgentState &state, int used, int size)
{
    if (size <= 0)
    {
        return;
    }
    state.ui->set_context(used, size);
}

inline void show_context(AgentState &state, const SessionResponse &session)
{
    show_context(state, session.context_used, session.context_size);
}
