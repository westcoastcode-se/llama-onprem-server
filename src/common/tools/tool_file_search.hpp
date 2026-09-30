#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kFileSearchName = "file_search";
inline constexpr std::string_view kFileSearchDescription =
    "Recursively search for files or directories whose name contains a substring. Skips vendors and build directories.";
inline constexpr std::string_view kFileSearchSchema =
    "arguments:\n"
    "      pattern: string (substring matched against the file or directory name)\n"
    "      path: string (optional starting directory, defaults to '.')";

Tool create_file_search_tool();

/**
 * @brief Direct execution function for "file_search".
 * @param args JSON object containing the "pattern" and optional "path" fields.
 * @return String with matching paths or error description.
 */
std::string file_search(const nlohmann::json & args);

} // namespace Tools
