#pragma once

#include "sessions.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * Session state
 */
struct JobState
{
    enum Value : int32_t
    {
        Unknown = -1,
        Queued,
        Running,
        Done,
        Error,
        Cancelled
    } value{Unknown};

    JobState() = default;
    JobState(const Value value) : value{value}
    {
    }

    bool operator==(const Value &v) const
    {
        return value == v;
    }

    bool operator != (const Value &v) const
    {
        return value != v;
    }

    /**
     * @return Check to see if the job is in a finished state
     */
    [[nodiscard]] bool is_finished() const
    {
        return value == Done || value == Error || value == Cancelled;
    }

    [[nodiscard]] const char *to_string() const
    {
        switch (value)
        {
        case Queued:
            return "queued";
        case Running:
            return "running";
        case Done:
            return "done";
        case Error:
            return "error";
        case Cancelled:
            return "cancelled";
        default:
            throw std::runtime_error{std::format("unknown JobState: {}", std::to_underlying(value))};
        }
    }

    static JobState to_enum(const std::string &s)
    {
        if (s == "queued")
        {
            return Queued;
        }
        if (s == "running")
        {
            return  Running;
        }
        if (s == "done")
        {
            return Done;
        }
        if (s == "error")
        {
            return Error;
        }
        if (s == "cancelled")
        {
            return Cancelled;
        }
        throw std::runtime_error{"unknown JobState: " + s};
    }
};

struct ChatTool
{
    std::string name;
    std::string description;
    // JSON object. Empty means an object with no properties.
    std::string parameters;

    [[nodiscard]] nlohmann::json to_json() const;

    static ChatTool from_json(const nlohmann::json &j);

};

/** Structured tool call. Codex sends these; the server does not run them. */
struct ParsedToolCall
{
    std::string id;
    std::string name;
    nlohmann::json arguments = nlohmann::json::object();

    [[nodiscard]] nlohmann::json to_json() const;

    static ParsedToolCall from_json(const nlohmann::json &j);
};

struct ChatMessage
{
    static constexpr std::string_view ROLE_ASSISTANT = "assistant";
    static constexpr std::string_view ROLE_USER = "user";
    static constexpr std::string_view ROLE_SYSTEM = "system";
    static constexpr std::string_view ROLE_TOOL = "tool";

    std::string role;
    std::string content;
    // Think body kept apart from content so the next turn can round-trip the template.
    std::string reasoning_content;
    // Set on a tool-role message. The chat template uses them to place the result.
    std::string tool_call_id;
    std::string tool_name;
    // Calls on an assistant message. Arguments stay JSON so the template can render them.
    std::vector<ParsedToolCall> tool_calls;

    /**
     * Validate required properties. Assistant content may be empty when the turn
     * was only reasoning, or the model stopped before writing an answer.
     */
    void validate() const
    {
        if (role.empty())
            throw BadRequest{"property 'role' is required"};
    }

    static ChatMessage from_json(const nlohmann::json &j);

    [[nodiscard]] nlohmann::json to_json() const;

};

/**
 * POST /v1/sessions
 */
struct CreateSessionRequest
{
    // Zero creates a session. Any other id resumes that session and leaves its conversation unchanged.
    SessionID id = 0;

    // Extra instructions appended to the system prompt the server builds for the active model.
    std::string system;

    // Chat history, if any exists. This is useful if you want to
    // recreate an old session
    std::vector<ChatMessage> messages;

    /**
     * When true, the server prompt includes the question protocol and a parsed <question> pauses the session.
     * The client replies with a normal POST .../messages turn.
     */
    bool questions = true;

    // When true, a finished turn's long tool results become one line before the next user message.
    bool compress_tools = true;

    // Model-neutral tools. The server renders them into the system prompt for the loaded model.
    std::vector<ChatTool> tools;

    // < 0 means the server default (no cap unless LlamaConfig sets one).
    int max_tokens = -1;

    void validate() const
    {
    }

    [[nodiscard]] nlohmann::json to_json() const;

    static CreateSessionRequest from_json(const nlohmann::json &j);

};

/** Model asks the user a question with optional multiple-choice answers. */
struct ParsedQuestion
{
    std::string text;
    std::vector<std::string> answers; // options; empty = free-form

    [[nodiscard]] nlohmann::json to_json() const;

    static ParsedQuestion from_json(const nlohmann::json &j);

};

/** GET /v1/sessions/:id/jobs/:key */
struct MessageStatusResponse
{
    JobKey key = 0;
    JobState state = JobState::Unknown;
    bool done = false;
    std::string content;
    std::string error;
    std::string error_code;
    std::string reasoning;
    std::vector<ParsedToolCall> tool_calls;
    std::optional<ParsedQuestion> question;

    void validate() const
    {
        if (key == 0)
            throw BadRequest{"property 'key' is required"};
        if (state.value == JobState::Unknown)
            throw BadRequest{"property 'state' is required"};
    }

    [[nodiscard]] nlohmann::json to_json() const;

    static MessageStatusResponse from_json(const nlohmann::json &j);

};

/**
 * Response from APIs:
 *
 * 1. POST /v1/sessions
 * 2. GET /v1/sessions/:id
 */
struct SessionResponse
{
    // Unique ID for this session
    SessionID id = 0;
    // The system prompt
    std::string system_prompt;
    std::vector<ChatMessage> messages;
    std::optional<JobKey> active_job_key;
    SessionState state;

    // Pending tool calls
    std::vector<ParsedToolCall> pending_tool_calls;

    // Pending question
    std::optional<ParsedQuestion> pending_question;

    // The LLM has asked a question and is waiting for a response.
    // TODO: This should be part of the client and not the server
    bool questions = true;

    // Finished tool results are shortened before the next user message.
    bool compress_tools = true;

    // Set when the latest generation failed. Empty after a successful turn.
    std::string error;
    // Stable code such as "context_full". Empty when error is empty or unclassified.
    std::string error_code;

    // Tokens currently stored in this session's KV cache, and the context window length.
    int context_used = 0;
    int context_size = 0;

    // GET /v1/sessions/{id} leaves this false and omits messages. Create and snapshot keep them.
    bool include_messages = true;

    /**
     * Validate required properties
     */
    void validate() const
    {
        if (id == 0)
            throw BadRequest{"property 'id' is required"};
        if (state.value == SessionState::Unknown)
            throw BadRequest{"property 'state' is required"};
    }

    [[nodiscard]] nlohmann::json to_json() const;

    static SessionResponse from_json(const nlohmann::json &j);

};

// Body of GET /v1/sessions. Headers only, same shape as GET /v1/sessions/{id}.
struct SessionListResponse
{
    std::vector<SessionResponse> sessions;

    [[nodiscard]] nlohmann::json to_json() const;

    static SessionListResponse from_json(const nlohmann::json &j);
};

// Body of GET /v1/sessions/{id}/messages. The session GET does not carry this list.
struct SessionMessagesResponse
{
    std::vector<ChatMessage> messages;

    [[nodiscard]] nlohmann::json to_json() const;

    static SessionMessagesResponse from_json(const nlohmann::json &j);
};
