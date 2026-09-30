#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kWriteFileName = "write_file";
inline constexpr std::string_view kWriteFileDescription = "Create a file or replace its entire contents. To change part of an existing file, use apply_patch.";
inline constexpr std::string_view kWriteFileSchema =
    "arguments:\n"
    "      path: string (path to the file)\n"
    "      content: string (the full content to write)";

Tool create_write_file_tool();

/**
 * @brief Direct execution function for "write_file".
 * @param args JSON object containing the "path" and "content" fields.
 * @return String with success status or error description.
 */
std::string write_file(const nlohmann::json & args);

} // namespace Tools
