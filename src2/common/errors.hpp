#pragma once
#include <exception>

namespace callisto {
    /**
     *
     */
    struct base_error : std::exception {
    };


    /**
     * Error that happens if a client authentication failed
     */
    struct auth_error : base_error {
        [[nodiscard]] const char *what() const noexcept final {
            return "auth_error";
        }
    };

    /**
     * Abort the current processor
     */
    struct abort : base_error {
        [[nodiscard]] const char *what() const noexcept final {
            return "abort";
        }
    };
}
