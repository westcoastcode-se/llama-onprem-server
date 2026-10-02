#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

// Devstral writes one call as [TOOL_CALLS]name[ARGS]{...}. An older form is
// [TOOL_CALLS] followed directly by a JSON array or object.
inline constexpr std::string_view kDevstralToolCalls = "[TOOL_CALLS]";
inline constexpr std::string_view kDevstralToolArgs = "[ARGS]";

// Index just past one JSON object or array. npos when the value is unfinished
// or does not start with '{' or '['.
inline std::size_t json_value_end(std::string_view text, std::size_t from)
{
    while (from < text.size() &&
           std::isspace(static_cast<unsigned char>(text[from])) != 0)
    {
        ++from;
    }
    if (from >= text.size() || (text[from] != '{' && text[from] != '['))
    {
        return std::string_view::npos;
    }

    int braces = 0;
    int brackets = 0;
    bool in_string = false;
    bool escape = false;
    for (std::size_t i = from; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (in_string)
        {
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\')
            {
                escape = true;
                continue;
            }
            if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (ch == '"')
        {
            in_string = true;
            continue;
        }
        if (ch == '{')
        {
            ++braces;
        }
        else if (ch == '}')
        {
            if (braces == 0)
            {
                return std::string_view::npos;
            }
            --braces;
        }
        else if (ch == '[')
        {
            ++brackets;
        }
        else if (ch == ']')
        {
            if (brackets == 0)
            {
                return std::string_view::npos;
            }
            --brackets;
        }
        if (braces == 0 && brackets == 0)
        {
            return i + 1;
        }
    }
    return std::string_view::npos;
}

// `from` must sit on [TOOL_CALLS]. The result is the index just past that call.
inline std::size_t devstral_call_end(std::string_view text, std::size_t from)
{
    if (from > text.size() || text.size() - from < kDevstralToolCalls.size() ||
        !text.substr(from).starts_with(kDevstralToolCalls))
    {
        return std::string_view::npos;
    }
    std::size_t cursor = from + kDevstralToolCalls.size();
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
    {
        ++cursor;
    }
    if (cursor >= text.size())
    {
        return std::string_view::npos;
    }
    if (text[cursor] == '{' || text[cursor] == '[')
    {
        return json_value_end(text, cursor);
    }
    const std::size_t args = text.find(kDevstralToolArgs, cursor);
    if (args == std::string_view::npos)
    {
        return std::string_view::npos;
    }
    return json_value_end(text, args + kDevstralToolArgs.size());
}
