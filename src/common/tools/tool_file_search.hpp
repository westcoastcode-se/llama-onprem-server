#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "file_search".
 *
 * Recursively searches directory structures for files or subdirectories
 * whose name or path matches a given substring pattern.
 * Skips inaccessible directories via `skip_permission_denied` and limits
 * search results to a maximum of 100 matches.
 *
 * JSON Parameters:
 *   - pattern (string, required): Substring pattern to match in filenames/paths.
 *   - path (string, optional, defaults to "."): Starting root directory.
 *
 * Return value:
 *   - List of matching paths (with [DIR] suffix for directories), or "no matching files found".
 */
Tool create_file_search_tool();

/**
 * @brief Direct execution function for "file_search".
 * @param args JSON object containing the "pattern" and optional "path" fields.
 * @return String with matching paths or error description.
 */
std::string file_search(const nlohmann::json & args);

} // namespace Tools
