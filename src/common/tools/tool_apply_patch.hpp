#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

// Replace one unique old_string in an existing file. Fails when the text is missing or repeated.
Tool create_apply_patch_tool();

std::string apply_patch(const nlohmann::json & args);

} // namespace Tools
