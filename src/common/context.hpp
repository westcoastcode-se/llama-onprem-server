#pragma once

#include "common/color.hpp"
#include "common/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Context {

inline constexpr float DEFAULT_COMPACT_THRESHOLD = 0.75f; // Auto-compact when >= 75% full

inline bool should_compact(int used_ctx, int n_ctx, float threshold = DEFAULT_COMPACT_THRESHOLD) {
    if (n_ctx <= 0 || used_ctx <= 0) return false;
    return (static_cast<float>(used_ctx) / static_cast<float>(n_ctx)) >= threshold;
}

inline float get_usage_percentage(int used_ctx, int n_ctx) {
    if (n_ctx <= 0) return 0.0f;
    return (static_cast<float>(used_ctx) / static_cast<float>(n_ctx)) * 100.0f;
}

inline std::string format_context_usage_summary(int used_ctx, int n_ctx) {
    float pct = get_usage_percentage(used_ctx, n_ctx);
    return std::format("{} / {} tokens ({:.1f}%)", used_ctx, n_ctx, pct);
}

inline std::string format_client_context_log(std::string_view client_ip, int client_port, int used_ctx, int n_ctx, std::string_view action = "") {
    float pct = get_usage_percentage(used_ctx, n_ctx);
    if (action.empty()) {
        return std::format("[server] Client {}:{} context: {} / {} tokens ({:.1f}%)",
                           std::string(client_ip), client_port, used_ctx, n_ctx, pct);
    } else {
        return std::format("[server] Client {}:{} {} context: {} / {} tokens ({:.1f}%)",
                           std::string(client_ip), client_port, std::string(action), used_ctx, n_ctx, pct);
    }
}

inline std::string format_progress_bar(int used_ctx, int n_ctx, int bar_width = 20) {
    if (n_ctx <= 0) return "";
    float ratio = std::clamp(static_cast<float>(used_ctx) / static_cast<float>(n_ctx), 0.0f, 1.0f);
    int filled = static_cast<int>(std::round(ratio * bar_width));
    std::string bar = "[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) {
            bar += "=";
        } else {
            bar += "-";
        }
    }
    bar += "]";
    return bar;
}

inline std::string build_summary_prompt(std::span<const Protocol::ChatMessage> msgs) {
    std::string prompt = "Summarize the following previous conversation history and actions concisely. "
                         "Preserve all key goals, user instructions, file names, tool observations, errors, and current task state:\n\n";
    for (const auto & m : msgs) {
        prompt += std::format("[{}]: {}\n\n", m.role, m.content);
    }
    prompt += "Concise Summary:";
    return prompt;
}

inline std::string create_fallback_summary(std::span<const Protocol::ChatMessage> msgs) {
    std::string out = std::format("Summary of previous interactions ({} messages):\n", msgs.size());
    for (const auto & m : msgs) {
        std::string snippet = m.content;
        if (snippet.size() > 150) {
            snippet = snippet.substr(0, 150) + "...";
        }
        std::replace(snippet.begin(), snippet.end(), '\n', ' ');
        out += std::format("- [{}]: {}\n", m.role, snippet);
    }
    return out;
}

inline bool compact_messages(std::vector<Protocol::ChatMessage> & messages,
                             std::string_view summary,
                             size_t keep_recent = 2) {
    if (messages.size() <= 2) {
        return false;
    }

    Protocol::ChatMessage sys_msg = messages.front();
    std::vector<Protocol::ChatMessage> recent_msgs;
    size_t start_recent = messages.size() > keep_recent + 1 ? messages.size() - keep_recent : 1;
    recent_msgs.reserve(messages.size() - start_recent);
    for (size_t i = start_recent; i < messages.size(); ++i) {
        recent_msgs.push_back(std::move(messages[i]));
    }

    messages.clear();
    messages.push_back(std::move(sys_msg));
    messages.push_back({"user", std::format("[Context Summary of earlier conversation:\n{}\n]", std::string(summary))});
    messages.push_back({"assistant", "Understood. I will continue with the context from the earlier conversation."});
    for (auto & m : recent_msgs) {
        messages.push_back(std::move(m));
    }
    return true;
}

inline void print_context_info(int used_ctx, int n_ctx, std::span<const Protocol::ChatMessage> messages) {
    float pct = get_usage_percentage(used_ctx, n_ctx);
    std::string bar = format_progress_bar(used_ctx, n_ctx, 24);
    int free_ctx = std::max(0, n_ctx - used_ctx);

    size_t sys_count = 0, user_count = 0, asst_count = 0, tool_count = 0, total_chars = 0;
    for (const auto & m : messages) {
        total_chars += m.content.size();
        if (m.role == "system") sys_count++;
        else if (m.role == "user") user_count++;
        else if (m.role == "assistant") asst_count++;
        else if (m.role == "tool") tool_count++;
    }

    const char * status_color = Color::GREEN;
    if (pct >= 85.0f) status_color = Color::RED;
    else if (pct >= 70.0f) status_color = Color::YELLOW;

    printf("\n%sContext Usage Overview:%s\n", Color::BOLD, Color::RESET);
    printf("  Tokens       : %s%d%s / %d (%.1f%%) %s%s%s\n",
           status_color, used_ctx, Color::RESET, n_ctx, pct,
           status_color, bar.c_str(), Color::RESET);
    printf("  Remaining    : %d tokens\n", free_ctx);
    printf("  Messages     : %zu total (sys: %zu, user: %zu, assistant: %zu, tool: %zu)\n",
           messages.size(), sys_count, user_count, asst_count, tool_count);
    printf("  Memory Size  : ~%zu characters\n", total_chars);
    if (pct >= 75.0f) {
        printf("  %sRecommendation: Run /compact to compress conversation context.%s\n", Color::YELLOW, Color::RESET);
    }
    printf("\n");
}

} // namespace Context
