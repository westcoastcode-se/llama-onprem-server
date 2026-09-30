#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kListDirectoryName = "list_directory";
inline constexpr std::string_view kListDirectoryDescription = "List files and subdirectories in a directory path.";
inline constexpr std::string_view kListDirectorySchema =
    "arguments:\n      path: string (optional directory path, defaults to '.')";

Tool create_list_directory_tool();

/**
 * @brief Direct execution function for "list_directory".
 * @param args JSON object containing the optional "path" field.
 * @return String with listed files/directories or error description.
 */
std::string list_directory(const nlohmann::json & args);

} // namespace Tools
