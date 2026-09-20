#pragma once

#include "std.hpp"
#include <experimental/scope>

#define CONCAT_IMPL(A, B) A##B
#define CONCAT(A, B) CONCAT_IMPL(A, B)
#define UNIQUE_ID(NAME) CONCAT(NAME, __COUNTER__)

#define defer(FUNC)                                                                                                    \
    auto UNIQUE_ID(_d) = std::experimental::scope_exit                                                                 \
    {                                                                                                                  \
        [&] { FUNC; }                                                                                                  \
    }

#define errdefer(FUNC)                                                                                                 \
    auto UNIQUE_ID(_d) = std::experimental::scope_fail                                                                 \
    {                                                                                                                  \
        [&] FUNC                                                                                                       \
    }
