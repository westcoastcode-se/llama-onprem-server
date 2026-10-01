#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

/**
 * Error thrown when a REST API is not found
 */
struct NotFound : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/**
 * Error thrown when the REST API request och response is invalid
 */
struct BadRequest : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Prompt or generation did not fit. The server removes the message that caused the turn.
inline constexpr std::string_view kContextFull = "context_full";

/**
 * Generic error response
 */
struct ErrorResponse
{
    // Error code
    int error_code;
    // Error message
    std::string message;

    [[nodiscard]] nlohmann::json to_json() const
    {
        // clang-format off
        return nlohmann::json
        {
            { "error_code", error_code },
            { "message", message }
        };
        // clang-format on
    }

    static ErrorResponse from_json(const nlohmann::json &j)
    {
        // clang-format off
        return ErrorResponse
        {
            .error_code = j.value("error_code", -1),
            .message = j.value("message", std::string()),
        };
        // clang-format on
    }
};