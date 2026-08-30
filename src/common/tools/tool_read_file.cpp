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

    std::string line;
    int current_line = 1;
    int lines_read = 0;
    std::string result;
    bool truncated = false;

    // Read line by line and format with line numbers
    while (std::getline(file, line)) {
        if (current_line >= offset && lines_read < limit) {
            result += std::to_string(current_line) + ": " + line + "\n";
            lines_read++;
            if (result.size() > MAX_TOOL_OUTPUT_CHARS) {
                truncated = true;
                break;
            }
        }
        current_line++;
    }

    if (lines_read == 0 && current_line <= offset) {
        return "error: offset " + std::to_string(offset) + " is beyond file length (" + std::to_string(current_line - 1) + " lines)";
    }

    if (result.empty()) {
        return "(empty file)";
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
