#pragma once

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
