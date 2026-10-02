#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kSearchTextName = "search_text";
inline constexpr std::string_view kSearchTextDescription =
    "Search for text across files in the project. method is text, extended, or regex. "
    "extended treats text1|text2 as either alternative, and * as any text, so Hej*123|std::string_view matches either.";
inline constexpr std::string_view kSearchTextSchema =
    "arguments:\n"
    "      query: string (text to find)\n"
    "      path: string (optional directory or file to search in, default '.')\n"
    "      file_pattern: string (optional filename filter or extension, e.g. '.cpp')\n"
    "      case_sensitive: boolean (optional, default false)\n"
    "      method: string (optional, text, extended, or regex; default text. extended treats text1|text2 as either alternative, and * as any text, for example Hej*123|std::string_view)\n"
    "      before: integer (optional lines before each hit, default 3. A line covered by several hits is listed once)\n"
    "      context: integer (optional lines after each hit, default 2, max 5)\n"
    "      max_matches: integer (optional, default 100)";

Tool create_search_text_tool();

/**
 * @brief Direct execution function for "search_text".
 * @param args JSON object with "query"/"pattern" parameters, and optional "path", "file_pattern", etc.
 * @return String with matching lines and line numbers or error description.
 */
std::string search_text(const nlohmann::json & args);

} // namespace Tools
