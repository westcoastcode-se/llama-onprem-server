#include "common/tools/tool_read_file.hpp"
#include <fstream>
#include <string>

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

    int current_line = 1;
    int lines_read = 0;
    std::string result;
    bool truncated = false;
    bool any_byte = false;

    while (lines_read < limit) {
        std::string line;
        bool got = false;
        char ch = 0;
        while (file.get(ch)) {
            any_byte = true;
            got = true;
            if (ch == '\n') {
                break;
            }
            if (line.size() >= MAX_TOOL_OUTPUT_CHARS) {
                truncated = true;
                while (file.get(ch) && ch != '\n') {
                }
                break;
            }
            line.push_back(ch);
        }
        if (!got) {
            break;
        }
        if (current_line >= offset) {
            result += std::to_string(current_line) + ": " + line + "\n";
            lines_read++;
            if (result.size() > MAX_TOOL_OUTPUT_CHARS) {
                result.resize(MAX_TOOL_OUTPUT_CHARS);
                truncated = true;
                break;
            }
        }
        current_line++;
    }

    if (!any_byte) {
        if (offset <= 1) {
            return "(empty file)";
        }
        return "error: offset " + std::to_string(offset) + " is beyond file length (0 lines)";
    }
    if (file.bad()) {
        return "error: failed to read file '" + path + "'";
    }
    if (lines_read == 0) {
        return "error: offset " + std::to_string(offset) + " is beyond file length (" +
               std::to_string(current_line - 1) + " lines)";
    }
    if (truncated) {
        result += "\n[content truncated]";
    }
    return result;
}

Tool create_read_file_tool() {
    return {
        "read_file",
        "Read file contents with line numbers.",
        "arguments:\n      path: string (path to the file)\n      offset: integer (optional start line, 1-indexed, default 1)\n      limit: integer (optional maximum lines to read, default 500)",
        read_file
    };
}

} // namespace Tools
