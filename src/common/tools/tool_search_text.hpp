#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "search_text".
 *
 * Recursively searches files in a directory (or a single file) for lines
 * matching a given search query or regular expression.
 * Automatically skips binary files and hidden VCS directories like .git.
 *
 * JSON Parameters:
 *   - query (string, required) or pattern: Search text or regex to match against file contents.
 *   - path (string, optional, defaults to "."): Root path or individual file to search in.
 *   - file_pattern (string, optional): Filename filter/extension (e.g. ".cpp", ".h", "CMakeLists.txt").
 *   - case_sensitive (bool, optional, defaults to false): Whether search is case-sensitive.
 *   - is_regex (bool, optional, defaults to false): Whether the query should be interpreted as a regex.
 *   - max_matches (integer, optional, defaults to 100): Maximum matching lines to return.
 *
 * Return value:
 *   - Formatted matches in "filepath:line_number: line_text" format or no matches message.
 */
Tool create_search_text_tool();

/**
 * @brief Direct execution function for "search_text".
 * @param args JSON object with "query"/"pattern" parameters, and optional "path", "file_pattern", etc.
 * @return String with matching lines and line numbers or error description.
 */
std::string search_text(const nlohmann::json & args);

} // namespace Tools
