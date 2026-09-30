#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

inline constexpr std::string_view kEditFileName = "edit_file";
inline constexpr std::string_view kEditFileDescription =
    "Create a file or change an existing one by applying a unified diff. "
    "A new file is a patch of only + lines, with or without a --- /dev/null header. "
    "An existing file is changed by hunks found from their context lines. "
    "Every hunk starts with @@ -start,count +start,count @@. "
    "Wrong numbers still apply when the surrounding lines match. "
    "A context line starts with a space, a removed line with '-', and an added line with '+'. "
    "Keep a few unchanged lines around every change. One call changes one file and may contain several hunks.\n"
    "New file:\n"
    "--- /dev/null\n"
    "+++ b/src/app.cpp\n"
    "@@ -0,0 +1,2 @@\n"
    "+int left = 1;\n"
    "+int right = 2;\n"
    "Change:\n"
    "@@ -12,3 +12,3 @@\n"
    " int left = 1;\n"
    "-int right = 2;\n"
    "+int right = 3;\n"
    " return left + right;";
inline constexpr std::string_view kEditFileSchema =
    "arguments:\n"
    "      path: string (file to create or change)\n"
    "      patch: string (unified diff. Each hunk starts with @@ -start,count +start,count @@. A new file has only + lines. Context lines start with a space)";

Tool create_edit_file_tool();

std::string edit_file(const nlohmann::json &args);

} // namespace Tools
