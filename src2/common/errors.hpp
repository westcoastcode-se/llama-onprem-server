#pragma once
#include <exception>

namespace callisto {
    struct base_error : std::exception {
    };


    /**
     * Error that happens if a client authentication failed
     */
    struct auth_error : base_error {
        const char *what() const noexcept override {
            return "authentication failed";
        }
    };
}
