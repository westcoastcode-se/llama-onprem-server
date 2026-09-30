#include "common/tools/tool_read_file.hpp"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace Tools {

std::string read_file(const nlohmann::json & args) {
    if (!args.contains("path") || !args["path"].is_string()) {
        return "error: missing required string argument 'path'";
    }
    std::string path = args["path"].get<std::string>();
    std::ifstream file(path);
    if (!file.is_open()) {
        return "error: could not open file '" + path + "'";
    }

    int offset = 1;
    int limit = 500;
    if (args.contains("offset") && args["offset"].is_number()) {
        offset = args["offset"].get<int>();
    }
    if (args.contains("limit") && args["limit"].is_number()) {
        limit = args["limit"].get<int>();
    }
    if (offset < 1) offset = 1;
    if (limit < 1) limit = 1;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() > MAX_TOOL_OUTPUT_CHARS) {
            line.resize(MAX_TOOL_OUTPUT_CHARS);
        }
        lines.push_back(std::move(line));
    }
    if (file.bad()) {
        return "error: failed to read file '" + path + "'";
    }
    if (lines.empty()) {
        if (offset <= 1) {
            return "(empty file)";
        }
        return "error: offset " + std::to_string(offset) + " is beyond file length (0 lines)";
    }
    if (offset > static_cast<int>(lines.size())) {
        return "error: offset " + std::to_string(offset) + " is beyond file length (" +
               std::to_string(lines.size()) + " lines)";
    }

    const int first = offset;
    int last = offset;
    std::string body;
    bool truncated = false;
    for (int number = offset; number <= static_cast<int>(lines.size()) && (number - offset) < limit; ++number) {
        body += std::to_string(number) + ": " + lines[static_cast<std::size_t>(number - 1)] + "\n";
        last = number;
        if (body.size() > MAX_TOOL_OUTPUT_CHARS) {
            body.resize(MAX_TOOL_OUTPUT_CHARS);
            truncated = true;
            break;
        }
    }
    std::string result = "lines " + std::to_string(first) + "-" + std::to_string(last) + " of " +
                         std::to_string(lines.size()) + "\n" + body;
    if (truncated) {
        result += "\n[content truncated]";
    }
    return result;
}

Tool create_read_file_tool() {
    return {
        .name = std::string(kReadFileName),
        .description = std::string(kReadFileDescription),
        .schema_doc = std::string(kReadFileSchema),
        .execute = read_file,
        .present = [](const nlohmann::json &args) { return tool_arg(args, "path"); }
    };
}

} // namespace Tools
