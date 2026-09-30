#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kReadFileName = "read_file";
inline constexpr std::string_view kReadFileDescription =
    "Read file contents with line numbers. The first line states the window, for example lines 1-80 of 420.";
inline constexpr std::string_view kReadFileSchema =
    "arguments:\n"
    "      path: string (path to the file)\n"
    "      offset: integer (optional start line, 1-indexed, default 1)\n"
    "      limit: integer (optional maximum lines to read, default 500)";

Tool create_read_file_tool();

/**
 * @brief Direct execution function for "read_file".
 * @param args JSON object with fields "path", and optional "offset" and "limit".
 * @return String with file lines or error description.
 */
std::string read_file(const nlohmann::json & args);

} // namespace Tools
