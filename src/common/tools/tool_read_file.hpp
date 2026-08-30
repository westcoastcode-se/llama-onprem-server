#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "read_file".
 *
 * Reads the content of a specified text file and prefixes each line with its
 * line number so that the language model can reference exact line numbers.
 * Supports pagination via `offset` (starting line) and `limit` (max lines).
 *
 * JSON Parameters:
 *   - path (string, required): Path to the file to read.
 *   - offset (integer, optional, defaults to 1): 1-indexed line number to start reading from.
 *   - limit (integer, optional, defaults to 500): Maximum number of lines to read.
 *
 * Return value:
 *   - Numbered lines ("1: line text\n2: line text\n..."), or error message.
 */
Tool create_read_file_tool();

/**
 * @brief Direct execution function for "read_file".
 * @param args JSON object with fields "path", and optional "offset" and "limit".
 * @return String with file lines or error description.
 */
std::string read_file(const nlohmann::json & args);

} // namespace Tools
