#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#define CONCAT_IMPL(A, B) A##B
#define CONCAT(A, B) CONCAT_IMPL(A, B)
#define UNIQUE_ID(NAME) CONCAT(NAME, __COUNTER__)

namespace defer_detail
{

// std::scope_exit / std::scope_fail are not in libstdc++ yet (they landed in C++26).
template <typename F> class scope_exit
{
    F fn_;
    bool active_ = true;

  public:
    explicit scope_exit(F fn) noexcept(std::is_nothrow_move_constructible_v<F>) : fn_(std::move(fn))
    {
    }

    scope_exit(scope_exit &&other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(other.fn_)), active_(std::exchange(other.active_, false))
    {
    }

    scope_exit(const scope_exit &) = delete;
    scope_exit &operator=(const scope_exit &) = delete;
    scope_exit &operator=(scope_exit &&) = delete;

    ~scope_exit() noexcept
    {
        if (active_)
        {
            fn_();
        }
    }
};

template <typename F> class scope_fail
{
    F fn_;
    int uncaught_ = std::uncaught_exceptions();
    bool active_ = true;

  public:
    explicit scope_fail(F fn) noexcept(std::is_nothrow_move_constructible_v<F>) : fn_(std::move(fn))
    {
    }

    scope_fail(scope_fail &&other) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(other.fn_)), uncaught_(other.uncaught_), active_(std::exchange(other.active_, false))
    {
    }

    scope_fail(const scope_fail &) = delete;
    scope_fail &operator=(const scope_fail &) = delete;
    scope_fail &operator=(scope_fail &&) = delete;

    ~scope_fail() noexcept
    {
        if (active_ && std::uncaught_exceptions() > uncaught_)
        {
            fn_();
        }
    }
};

template <typename F> scope_exit(F) -> scope_exit<F>;
template <typename F> scope_fail(F) -> scope_fail<F>;

} // namespace defer_detail

#define defer(FUNC)                                                                                                    \
    auto UNIQUE_ID(_d) = ::defer_detail::scope_exit                                                                    \
    {                                                                                                                  \
        [&] { FUNC; }                                                                                                  \
    }

#define errdefer(FUNC)                                                                                                 \
    auto UNIQUE_ID(_d) = ::defer_detail::scope_fail                                                                    \
    {                                                                                                                  \
        [&] FUNC                                                                                                       \
    }
