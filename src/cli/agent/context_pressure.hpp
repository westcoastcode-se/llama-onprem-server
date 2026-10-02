#pragma once

#include <string>
#include <string_view>

// Offer /compact once the filled fraction is strictly above this.
inline constexpr int kCompactOfferPercent = 80;

// True when used/size is strictly above percent. An unknown window (size <= 0) never matches.
[[nodiscard]] inline bool context_above(int used, int size, int percent)
{
    if (size <= 0 || percent <= 0)
    {
        return false;
    }
    const long long tokens = used < 0 ? 0 : used;
    return tokens * 100 > static_cast<long long>(size) * percent;
}

// Rounded percent, same formula as the context meter. Caps at 999.
[[nodiscard]] inline int context_percent(int used, int size)
{
    if (size <= 0)
    {
        return 0;
    }
    const long long tokens = used < 0 ? 0 : used;
    long long percent = (100LL * tokens + size / 2) / size;
    if (percent > 999)
    {
        percent = 999;
    }
    return static_cast<int>(percent);
}

// The server is paused for a client reply and the window is past the offer threshold.
[[nodiscard]] inline bool should_offer_compact(int used, int size, bool server_waiting, bool interactive)
{
    return interactive && server_waiting && context_above(used, size, kCompactOfferPercent);
}

// A sub-agent has no dialog. It compacts on its own at the same threshold.
[[nodiscard]] inline bool should_auto_compact(int used, int size, bool server_waiting, bool subagent)
{
    return subagent && server_waiting && context_above(used, size, kCompactOfferPercent);
}

// Keep the start and the end. context_size is a character budget (one per token).
// Zero means the window is unknown, and the budget is then 8192 characters.
[[nodiscard]] inline std::string truncate_transcript(std::string_view transcript, int context_size)
{
    const int window = context_size > 0 ? context_size : 8192;
    const std::size_t budget = static_cast<std::size_t>(window);
    if (transcript.size() <= budget)
    {
        return std::string(transcript);
    }
    constexpr std::string_view kGap = "\n...[earlier conversation omitted]...\n";
    if (budget <= kGap.size())
    {
        return std::string(transcript.substr(0, budget));
    }
    const std::size_t head = budget / 5;
    const std::size_t tail = budget - head - kGap.size();
    return std::string(transcript.substr(0, head)) + std::string(kGap) +
           std::string(transcript.substr(transcript.size() - tail));
}
