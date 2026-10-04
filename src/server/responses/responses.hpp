#pragma once

#include "api/models.hpp"

#include <string>
#include <string_view>
#include <vector>

// One Responses turn from Codex or Copilot. The server generates. The client runs the tools.
struct ResponsesTurn
{
    // Echoed on the response. Empty when the request omitted model.
    std::string model;
    // System text from `instructions`. Jobs places it before the messages.
    std::string instructions;
    std::vector<ChatMessage> messages;
    std::vector<ChatTool> tools;
    // Names from tools of type namespace. Members are stored as namespace + "." + name.
    std::vector<std::string> tool_namespaces;
    int max_tokens = -1;
    // Negative keeps the server temperature.
    float temperature = -1.0f;
    bool stream = false;
    // KV slot. prompt_cache_key when the client sends one, otherwise "codex".
    std::string session_id;
};

// Throws BadRequest. previous_response_id is ignored: the client resends the input.
[[nodiscard]] ResponsesTurn responses_from_json(const nlohmann::json &body);

// GGUF file name without the directory or the .gguf suffix.
[[nodiscard]] std::string public_model_id(std::string_view model_path);

// Answer text with think and tool-call markup removed. Tool calls are separate items.
[[nodiscard]] std::string responses_visible_text(std::string_view raw);

struct ResponseIds
{
    std::string response;
    std::string reasoning;
    std::string message;

    [[nodiscard]] static ResponseIds make();
};

// What the worker produced, after the model adapter has parsed tool calls.
struct ResponsesResult
{
    enum class Status
    {
        Completed,
        Failed,
        Cancelled
    };

    Status status = Status::Completed;
    std::string reasoning;
    std::string text;
    std::vector<ParsedToolCall> tool_calls;
    std::string error;
    std::string error_code;
    // Copied from the request so a qualified tool name can be split back into namespace + name.
    std::vector<std::string> tool_namespaces;
    std::vector<ChatTool> tools;
};

// Output items the client stores from response.output_item.done.
// A function_call carries namespace when the name matches a declared namespace.
[[nodiscard]] std::vector<nlohmann::json> response_output_items(const ResponseIds &ids, const ResponsesResult &result);

// Terminal SSE payloads (the JSON `data`, not the frame). The stream ends on one of these.
[[nodiscard]] std::vector<nlohmann::json> response_terminal_events(const ResponseIds &ids, std::string_view model,
                                                                   const ResponsesResult &result);

[[nodiscard]] std::string sse_frame(const nlohmann::json &data);

// True when Authorization is "Bearer <expected_key>". The scheme is case-insensitive.
// An empty expected key never matches. The key bytes are compared without an early exit.
[[nodiscard]] bool authorization_matches(std::string_view expected_key, std::string_view authorization_header);

// Live deltas. Tool-call markup is dropped. <think> is reasoning, not answer text.
class ResponsesDeltaFilter
{
  public:
    struct Delta
    {
        std::string reasoning;
        std::string text;
    };

    [[nodiscard]] Delta feed(std::string_view piece);
    [[nodiscard]] Delta finish();

  private:
    enum class Mode
    {
        Text,
        Reasoning,
        Tool
    };

    [[nodiscard]] Delta drain(bool flush);

    std::string pending_;
    std::string tool_end_;
    Mode mode_ = Mode::Text;
};
