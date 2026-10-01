#include "cli/agent/tool_schema.hpp"

#include <string>
#include <string_view>

namespace
{

std::string_view trim_line(std::string_view text)
{
    constexpr std::string_view ws = " \t\r";
    const auto begin = text.find_first_not_of(ws);
    if (begin == std::string_view::npos)
    {
        return {};
    }
    const auto end = text.find_last_not_of(ws);
    return text.substr(begin, end - begin + 1);
}

} // namespace

nlohmann::json ToolSchema::from_prose(std::string_view schema_doc)
{
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required = nlohmann::json::array();
    std::size_t pos = 0;
    while (pos < schema_doc.size())
    {
        const std::size_t nl = schema_doc.find('\n', pos);
        const std::string_view line =
            trim_line(schema_doc.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? schema_doc.size() : nl + 1;
        if (line.empty() || line.starts_with("arguments"))
        {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
        {
            continue;
        }
        const std::string_view name = trim_line(line.substr(0, colon));
        if (name.empty() || name.find(' ') != std::string_view::npos)
        {
            continue;
        }
        const std::string_view rest = trim_line(line.substr(colon + 1));
        const std::size_t word_end = rest.find_first_of(" \t");
        const std::string_view type_word = rest.substr(0, word_end);
        std::string type = "string";
        if (type_word.starts_with("bool"))
        {
            type = "boolean";
        }
        else if (type_word.starts_with("int"))
        {
            type = "integer";
        }
        else if (type_word.starts_with("number"))
        {
            type = "number";
        }
        else if (type_word.starts_with("array"))
        {
            type = "array";
        }
        else if (type_word.starts_with("object"))
        {
            type = "object";
        }
        nlohmann::json spec = {{"type", type}};
        const std::size_t open = rest.find('(');
        const std::size_t close = rest.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open)
        {
            spec["description"] = std::string(trim_line(rest.substr(open + 1, close - open - 1)));
        }
        properties[std::string(name)] = std::move(spec);
        if (rest.find("optional") == std::string_view::npos)
        {
            required.push_back(std::string(name));
        }
    }
    nlohmann::json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty())
    {
        schema["required"] = std::move(required);
    }
    return schema;
}

ChatTool ToolSchema::chat_tool(const Tool &tool)
{
    ChatTool spec;
    spec.name = tool.name;
    spec.description = tool.description;
    const nlohmann::json schema = from_prose(tool.schema_doc);
    if (schema["properties"].empty() && !tool.schema_doc.empty())
    {
        spec.description.push_back('\n');
        spec.description += tool.schema_doc;
    }
    spec.parameters = schema.dump();
    return spec;
}
