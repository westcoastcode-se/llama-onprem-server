#include "common/tools.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{

struct BaseTool
{
    std::string_view name;
    ToolKind kind;
    Tool (*create)();
};

// The tools the agent can run, and how the client classifies each one.
constexpr BaseTool kBaseTools[] = {
    {Tools::kExecuteCommandName, ToolKind::Shell, Tools::create_execute_command_tool},
    {Tools::kReadFileName, ToolKind::Read, Tools::create_read_file_tool},
    {Tools::kWriteFileName, ToolKind::Write, Tools::create_write_file_tool},
    {Tools::kEditFileName, ToolKind::Write, Tools::create_edit_file_tool},
    {Tools::kListDirectoryName, ToolKind::Read, Tools::create_list_directory_tool},
    {Tools::kFileSearchName, ToolKind::Read, Tools::create_file_search_tool},
    {Tools::kSearchTextName, ToolKind::Read, Tools::create_search_text_tool},
    {Tools::kWebFetchName, ToolKind::Network, Tools::create_web_fetch_tool},
    {Tools::kWebSearchName, ToolKind::Network, Tools::create_web_search_tool},
};

} // namespace

ToolKind tool_kind(std::string_view name)
{
    for (const BaseTool &entry : kBaseTools)
    {
        if (entry.name == name)
        {
            return entry.kind;
        }
    }
    return ToolKind::Other;
}

std::vector<Tool> get_base_tools()
{
    std::vector<Tool> tools;
    tools.reserve(std::size(kBaseTools));
    for (const BaseTool &entry : kBaseTools)
    {
        tools.push_back(entry.create());
    }
    return tools;
}

std::vector<Tool> get_registered_tools(bool include_subagents, SubagentRunner subagent_runner)
{
    std::vector<Tool> tools = get_base_tools();
    if (include_subagents)
    {
        tools.push_back(Tools::create_subagent_tool(subagent_runner));
    }
    return tools;
}

std::string load_agents_markdown(const std::filesystem::path base_dir) {
    // AGENTS.md lives in the project root, not under .agents or .callisto.
    std::filesystem::path p = base_dir / "AGENTS.md";
    if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
        std::ifstream ifs(p);
        if (ifs.is_open()) {
            std::stringstream buffer;
            buffer << ifs.rdbuf();
            std::string content = buffer.str();
            // Trim trailing whitespace
            size_t last = content.find_last_not_of(" \t\r\n");
            if (last != std::string::npos) {
                content = content.substr(0, last + 1);
            } else {
                content.clear();
            }
            return content;
        }
    }

    return "";
}

const Tool *find_tool(std::span<const Tool> tools, std::string_view name)
{
    for (const Tool &tool : tools)
    {
        if (tool.name == name)
        {
            return &tool;
        }
    }
    for (const Tool &tool : tools)
    {
        for (const std::string &alias : tool.aliases)
        {
            if (alias == name)
            {
                return &tool;
            }
        }
    }
    return nullptr;
}

std::string run_tool(std::span<const Tool> tools, std::string_view name, const nlohmann::json & arguments) {
    const Tool *tool = find_tool(tools, name);
    if (tool == nullptr || !tool->execute)
    {
        return std::format("error: unknown tool '{}'", name);
    }
    try
    {
        return tool->execute(arguments);
    }
    catch (const std::exception & e)
    {
        return std::format("error executing tool '{}': {}", name, e.what());
    }
}
