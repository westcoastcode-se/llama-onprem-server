#include "common/tools/tool_list_directory.hpp"
#include <filesystem>
#include <string>

namespace Tools {

std::string list_directory(const nlohmann::json & args) {
    std::string path_str = ".";
    if (args.contains("path") && args["path"].is_string()) {
        path_str = args["path"].get<std::string>();
    }

    try {
        std::filesystem::path p(path_str);
        if (!std::filesystem::exists(p)) {
            return "error: directory does not exist: " + path_str;
        }
        if (!std::filesystem::is_directory(p)) {
            return "error: path is not a directory: " + path_str;
        }

        std::string result;
        int count = 0;
        for (const auto & entry : std::filesystem::directory_iterator(p)) {
            if (++count > 250) {
                result += "... [entries truncated]\n";
                break;
            }
            std::string type = entry.is_directory() ? "[DIR] " : "[FILE]";
            std::string size_str;
            if (entry.is_regular_file()) {
                size_str = " (" + std::to_string(entry.file_size()) + " bytes)";
            }
            result += type + " " + entry.path().filename().string() + size_str + "\n";
        }
        return result.empty() ? "(empty directory)" : result;
    } catch (const std::exception & e) {
        return std::string("error listing directory: ") + e.what();
    }
}

Tool create_list_directory_tool() {
    return {
        "list_directory",
        "List files and subdirectories in a directory path.",
        "arguments:\n      path: string (optional directory path, defaults to '.')",
        list_directory
    };
}

} // namespace Tools
