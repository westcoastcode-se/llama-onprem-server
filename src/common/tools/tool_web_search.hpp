#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "web_search".
 *
 * Searches the web via a local SearXNG meta-search engine (http://localhost:4488).
 * Supports search queries, categories, language/time filters, and result count limits.
 * Parses SearXNG JSON responses and formats direct answers, infoboxes,
 * search results (with titles, URLs, and snippets), and query suggestions.
 *
 * JSON Parameters:
 *   - query / q / search_query / input (string, required): Search query or keywords.
 *   - limit / count / max_results (integer, optional, defaults to 5, max 20): Result count.
 *   - categories (string, optional): SearXNG categories (e.g. "general", "science", "it").
 *   - language (string, optional): Language code (e.g. "sv", "en").
 *   - time_range (string, optional): Time filter (e.g. "day", "week", "month", "year").
 *
 * Return value:
 *   - Formatted list of search results or an error message.
 */
Tool create_web_search_tool();

/**
 * @brief Direct execution function for "web_search".
 * @param args JSON object with search parameters.
 * @return String with search results or error description.
 */
std::string web_search(const nlohmann::json & args);

} // namespace Tools
