#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

// message_ends are server-side end offsets into the cached token sequence, one per message
// that was rendered into that sequence. The longest end that is still a prefix of both
// sequences is kept, then the match continues token by token through any generated tail.
[[nodiscard]] inline size_t checkpoint_from_offsets(std::span<const int32_t> active, std::span<const int32_t> prompt,
                                                    std::span<const size_t> message_ends)
{
    size_t matched = 0;
    for (const size_t end : message_ends)
    {
        if (end < matched || end > active.size() || end > prompt.size())
        {
            break;
        }
        if (!std::equal(active.begin() + static_cast<std::ptrdiff_t>(matched),
                        active.begin() + static_cast<std::ptrdiff_t>(end),
                        prompt.begin() + static_cast<std::ptrdiff_t>(matched)))
        {
            break;
        }
        matched = end;
    }

    const size_t n = std::min(active.size(), prompt.size());
    for (size_t i = matched; i < n; ++i)
    {
        if (active[i] != prompt[i])
        {
            break;
        }
        matched = i + 1;
    }
    return matched;
}
