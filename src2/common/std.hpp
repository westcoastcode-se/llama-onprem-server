#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <span>

namespace callisto {
    using string = std::string;
    using string_view = std::string;

    template<typename T>
    using span = std::span<T>;

    template<typename T>
    using unique_ptr = std::unique_ptr<T>;

    using bytes = std::span<std::byte>;
}
