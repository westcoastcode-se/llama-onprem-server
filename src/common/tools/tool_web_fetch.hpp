#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kWebFetchName = "web_fetch";
inline constexpr std::string_view kWebFetchDescription =
    "Fetch the readable text or html content of a web page via HTTP(S).";
inline constexpr std::string_view kWebFetchSchema =
    "arguments:\n"
    "      url: string (absolute http/https url)\n"
    "      type: string (optional, text or html, defaults to text)";

Tool create_web_fetch_tool();

/**
 * @brief Direct execution function for "web_fetch".
 * @param args JSON object with fields "url" and optional "type".
 * @return String with fetched content or error description.
 */
std::string web_fetch(const nlohmann::json & args);

} // namespace Tools
