#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kWebSearchName = "web_search";
inline constexpr std::string_view kWebSearchDescription = "Search the internet for queries, web pages, and information.";
inline constexpr std::string_view kWebSearchSchema =
    "arguments:\n"
    "      query: string (search keywords or question)\n"
    "      limit: integer (optional maximum results to return, default 5)\n"
    "      categories: string (optional SearXNG categories such as general, science, it)\n"
    "      language: string (optional language code such as sv or en)\n"
    "      time_range: string (optional day, week, month, or year)";

Tool create_web_search_tool();

/**
 * @brief Direct execution function for "web_search".
 * @param args JSON object with search parameters.
 * @return String with search results or error description.
 */
std::string web_search(const nlohmann::json & args);

} // namespace Tools
