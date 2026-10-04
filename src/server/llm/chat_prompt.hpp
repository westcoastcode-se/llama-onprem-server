#pragma once

#include "../../api/models.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct common_chat_templates;

// How format_messages feeds one model template. Devstral's Jinja rejects the shape Codex sends.
struct ChatPromptPolicy
{
    bool add_assistant = true;
    // Passed through as enable_thinking. Qwen opens a think block from this.
    bool enable_thinking = true;
    // False when the template has no reasoning channel. A reasoning field would be rewritten
    // into a thinking chunk that the template rejects.
    bool keep_reasoning = true;
    // True when the template counts only plain user and assistant turns and raises otherwise.
    // Codex sends the environment and the prompt as two user messages.
    bool merge_users = false;
};

// Devstral Small 2 raises when a user turn follows another user turn, or follows a tool
// result, because assistant tool calls do not advance its alternation counter.
[[nodiscard]] bool template_requires_alternating_roles(std::string_view source);

// The raise is removed. The same template still renders [INST], [TOOL_CALLS], and [TOOL_RESULTS].
[[nodiscard]] std::string relax_strict_role_template(std::string source);

// Qwen and DeepSeek templates mention a reasoning channel. Devstral does not.
[[nodiscard]] bool template_renders_reasoning(std::string_view source);

// Render the Jinja prompt. Throws std::runtime_error when the template cannot be applied.
[[nodiscard]] std::string render_chat_prompt(const common_chat_templates *templates,
                                             std::span<const ChatMessage> messages,
                                             std::span<const ChatTool> tools, const ChatPromptPolicy &policy);
