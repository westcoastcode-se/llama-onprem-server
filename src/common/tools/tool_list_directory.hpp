#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "list_directory".
 *
 * Lists files and subdirectories in a specified directory path.
 * Shows item type indicators ([DIR] or [FILE]) and size in bytes for regular files.
 * Limits the number of returned entries to 250 to avoid overly large outputs.
 *
 * JSON Parameters:
 *   - path (string, optional, defaults to "."): Directory path to list.
 *
 * Return value:
 *   - Formatted directory listing, or error message.
 */
Tool create_list_directory_tool();

/**
 * @brief Direct execution function for "list_directory".
 * @param args JSON object containing the optional "path" field.
 * @return String with listed files/directories or error description.
 */
std::string list_directory(const nlohmann::json & args);

} // namespace Tools
