#pragma once

#include <cstddef>
#include <cstdint>

// pos_max is the last position stored for the live sequence, or -1 when it is empty.
// A hybrid cache can report that a suffix was removed while its recurrent tail stayed
// behind the attention cells. The token count and that tail are the same sequence only
// when the tail sits on the last kept token.
[[nodiscard]] inline bool kv_tail_matches(size_t n_tokens, int32_t pos_max)
{
    if (n_tokens == 0)
    {
        return pos_max < 0;
    }
    return pos_max >= 0 && static_cast<size_t>(pos_max) + 1 == n_tokens;
}
