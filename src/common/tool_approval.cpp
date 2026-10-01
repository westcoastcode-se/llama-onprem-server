#include "common/color.hpp"
#include "common/tools.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

bool is_tool_allowed(const std::string_view tool_name, const bool auto_approve,
                     const std::span<const std::string> allowed_tools)
{
    if (auto_approve)
    {
        return true;
    }
    std::string lower_tool(tool_name);
    std::transform(lower_tool.begin(), lower_tool.end(), lower_tool.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto &allowed : allowed_tools)
    {
        if (allowed == "*" || allowed == "all" || allowed == lower_tool)
        {
            return true;
        }
    }
    return false;
}
