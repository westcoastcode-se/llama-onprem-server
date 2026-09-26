#pragma once

#include "api/models.hpp"
#include "common/std.hpp"
#include "common/tools.hpp"

// Turns a tool's prose schema_doc into the JSON schema sent to the server.
class ToolSchema
{
  public:
    [[nodiscard]] static json from_prose(std::string_view schema_doc);
    [[nodiscard]] static ChatTool chat_tool(const Tool &tool);
};
