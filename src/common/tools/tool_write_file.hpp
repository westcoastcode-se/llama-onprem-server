#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "write_file".
 *
 * Writes or overwrites a file on the filesystem with the provided text.
 * Parent directories are automatically created if they do not exist
 * via `std::filesystem::create_directories`.
 *
 * JSON Parameters:
 *   - path (string, required): Path to the file to create/overwrite.
 *   - content (string, required): The full text content to write.
 *
 * Return value:
 *   - Confirmation string with written byte count, or an error message.
 */
Tool create_write_file_tool();

/**
 * @brief Direct execution function for "write_file".
 * @param args JSON object containing the "path" and "content" fields.
 * @return String with success status or error description.
 */
std::string write_file(const nlohmann::json & args);

} // namespace Tools
