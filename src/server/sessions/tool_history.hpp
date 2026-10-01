#pragma once

#include <string>
#include <string_view>

// Opening tag for a stored tool result. Empty name and detail yield a plain tag.
// Detail is one line: quotes and brackets become spaces, and a newline ends it.
[[nodiscard]] std::string tool_response_open(std::string_view name, std::string_view detail);

// Replaces a long <tool_response> body with one line naming the tool and its target.
// A body of at most 240 characters and 3 lines is kept. A sub_agent result is kept in
// full, under the name the model called and under its aliases. A body that is already
// that one-line record, including an older "omitted ..." line, is kept. Text outside
// the tags is unchanged, and the opening tag is kept so the target is still available.
[[nodiscard]] std::string shrink_tool_responses(std::string_view text);
