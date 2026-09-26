#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

template <typename T> inline size_t common_prefix_length(std::span<const T> a, std::span<const T> b)
{
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    for (; i < n; ++i)
    {
        if (!(a[i] == b[i]))
        {
            break;
        }
    }
    return i;
}
