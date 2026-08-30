#include "common/tools/tool_file_search.hpp"
#include <filesystem>
#include <string>

namespace Tools {

std::string file_search(const nlohmann::json & args) {
    if (!args.contains("pattern") || !args["pattern"].is_string()) {
        return "error: missing required string argument 'pattern'";
    }
    std::string pattern = args["pattern"].get<std::string>();
    std::string start_path = ".";
    if (args.contains("path") && args["path"].is_string()) {
        start_path = args["path"].get<std::string>();
    }

    try {
        std::string result;
        int count = 0;
        // Recursive traversal skipping directories without read permissions
        for (const auto & entry : std::filesystem::recursive_directory_iterator(
                 start_path, std::filesystem::directory_options::skip_permission_denied)) {
            std::string filename = entry.path().filename().string();
            std::string path_str = entry.path().string();
            if (filename.find(pattern) != std::string::npos || path_str.find(pattern) != std::string::npos) {
                result += path_str + (entry.is_directory() ? " [DIR]" : "") + "\n";
                if (++count >= 100) {
                    result += "... [matches truncated]\n";
                    break;
                }
            }
        }
        return result.empty() ? "no matching files found" : result;
    } catch (const std::exception & e) {
        return std::string("error searching files: ") + e.what();
    }
}

Tool create_file_search_tool() {
    return {
        "file_search",
        "Recursively search for files or directories matching a name pattern.",
        "arguments:\n      pattern: string (substring to match)\n      path: string (optional starting directory, defaults to '.')",
        file_search
    };
}

} // namespace Tools
