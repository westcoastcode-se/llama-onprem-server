#pragma once

#include "cli/agent/agent_state.hpp"

#include "common/tools.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One interactive or exec session: open, slash commands, and the tool loop.
class AgentSession
{
  public:
    AgentSession(AgentState &state, const AgentConfig &config, std::vector<Tool> base_tools);
    AgentSession(const AgentSession &) = delete;
    AgentSession &operator=(const AgentSession &) = delete;

    [[nodiscard]] int loop();

  private:
    AgentState &state_;
    const AgentConfig &config_;
    std::vector<Tool> base_tools_;
    std::vector<Tool> tools_;
    // Set while a compact is summarizing, so that turn does not compact again.
    bool compacting_ = false;
    // A failed automatic compact must not run again on every later tool round.
    bool auto_compact_blocked_ = false;
    // Caps nested compact-and-continue so a full window cannot recurse.
    int compact_depth_ = 0;

    [[nodiscard]] std::string subagent(std::string_view task, bool inherit);
    [[nodiscard]] TurnStatus drive(JobKey key);
    TurnStatus submit(const std::string &prompt);
    [[nodiscard]] SessionID open(const AgentConfig &config);
    [[nodiscard]] SessionID open_with_history(std::vector<ChatMessage> history);
    [[nodiscard]] CreateSessionRequest make_request(const AgentConfig &config) const;
    [[nodiscard]] AgentConfig endpoint_config(const AgentConfig &config) const;
    void bind_session(SessionID id);
    [[nodiscard]] ServerSlot &slot();
    [[nodiscard]] const ServerSlot &slot() const;
    [[nodiscard]] std::optional<std::size_t> find_model(std::string_view query) const;
    void list_models();
    void use_model(std::size_t index);
    [[nodiscard]] bool model_command(std::string_view argument);
    [[nodiscard]] std::string offline_message() const;
    [[nodiscard]] bool ensure_server();
    [[nodiscard]] static std::string last_assistant(std::span<const ChatMessage> messages);
    void present_system(const SessionResponse &created) const;
    // Resume this id with POST /v1/sessions, remember it, and draw its transcript.
    // Throws when this computer did not create the id, or the server no longer has it.
    void present_transcript_of(const AgentConfig &stored, SessionID id);
    // Replace the transcript with this session's system prompt and messages.
    void present_transcript(const SessionResponse &header, std::span<const ChatMessage> messages) const;
    void resume_session();
    void refresh_status();
    // resume posts a follow-up so the new session keeps working. Otherwise the summary is the last step.
    [[nodiscard]] TurnStatus compact(bool resume);
    // install remembers the new id as this client's session. A sub-agent leaves the parent id in place.
    [[nodiscard]] TurnStatus compact_session(AgentState &state, std::span<const Tool> tools, bool resume, bool install);
    // Summary text, or an empty string. ContextFull means the transcript itself did not fit.
    [[nodiscard]] std::pair<TurnStatus, std::string> summarize_for_compact(AgentState &state, std::span<const Tool> tools);
    void help() const;
    void diff() const;
    [[nodiscard]] bool slash(const std::string &line);
};
