#include "common/tools/tool_apply_patch.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace Tools {
namespace {

std::string change_lines(const std::string &text, char mark)
{
    std::istringstream in(text);
    std::string line;
    std::string out;
    int shown = 0;
    bool any = false;
    while (std::getline(in, line))
    {
        any = true;
        if (shown == 20)
        {
            out += mark;
            out += " ...\n";
            return out;
        }
        out += mark;
        out += ' ';
        out += line;
        out += '\n';
        ++shown;
    }
    if (!any && !text.empty())
    {
        out += mark;
        out += ' ';
        out += text;
        out += '\n';
    }
    return out;
}

bool write_atomic(const std::filesystem::path &path, const std::string &content, std::string &error)
{
    const std::filesystem::path tmp = path.string() + ".callisto-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "error: failed to open file for writing: " + path.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            out.close();
            std::filesystem::remove(tmp);
            error = "error: failed to write file: " + path.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path);
    return true;
}

} // namespace

std::string apply_patch(const nlohmann::json &args)
{
    if (!args.contains("path") || !args["path"].is_string())
    {
        return "error: missing required string argument 'path'";
    }
    if (!args.contains("old_string") || !args["old_string"].is_string())
    {
        return "error: missing required string argument 'old_string'";
    }
    if (!args.contains("new_string") || !args["new_string"].is_string())
    {
        return "error: missing required string argument 'new_string'";
    }
    const std::string path_text = args["path"].get<std::string>();
    const std::string old_string = args["old_string"].get<std::string>();
    const std::string new_string = args["new_string"].get<std::string>();
    if (old_string.empty())
    {
        return "error: old_string must not be empty; use write_file to create a file";
    }

    std::ifstream in(path_text);
    if (!in.is_open())
    {
        return "error: could not open file '" + path_text + "'; use write_file to create it";
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
    {
        return "error: failed to read file '" + path_text + "'";
    }
    const std::string original = buffer.str();

    int count = 0;
    for (std::size_t pos = 0; (pos = original.find(old_string, pos)) != std::string::npos; pos += old_string.size())
    {
        ++count;
    }
    if (count == 0)
    {
        return "error: old_string not found in " + path_text;
    }
    if (count > 1)
    {
        return "error: old_string appears " + std::to_string(count) + " times in " + path_text +
               "; include more surrounding lines";
    }

    std::string updated = original;
    const auto at = updated.find(old_string);
    updated.replace(at, old_string.size(), new_string);

    std::string error;
    try
    {
        if (!write_atomic(path_text, updated, error))
        {
            return error;
        }
    }
    catch (const std::exception &ex)
    {
        return std::string("error writing file: ") + ex.what();
    }
    return "patched " + path_text + "\n" + change_lines(old_string, '-') + change_lines(new_string, '+');
}

Tool create_apply_patch_tool()
{
    return {
        .name = "apply_patch",
        .description = "Replace one unique stretch of text in an existing file. Fails if old_string is missing or appears more than once.",
        .schema_doc = "arguments:\n      path: string (path to the existing file)\n      old_string: string (exact text to replace, must occur once)\n      new_string: string (replacement text)",
        .execute = apply_patch,
        .present = [](const nlohmann::json &args) { return tool_arg(args, "path"); }};
}

} // namespace Tools
