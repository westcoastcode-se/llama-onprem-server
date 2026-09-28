#include "common/tools/tool_list_directory.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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

        struct Row {
            std::string name;
            bool directory = false;
            std::uintmax_t size = 0;
        };
        std::vector<Row> rows;
        for (const auto &entry : std::filesystem::directory_iterator(p)) {
            Row row;
            row.name = entry.path().filename().string();
            row.directory = entry.is_directory();
            if (entry.is_regular_file()) {
                row.size = entry.file_size();
            }
            rows.push_back(std::move(row));
        }
        std::ranges::sort(rows, [](const Row &left, const Row &right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            return left.name < right.name;
        });
        if (rows.empty()) {
            return "(empty directory)";
        }
        constexpr std::size_t kCap = 250;
        const std::size_t shown = std::min(rows.size(), kCap);
        std::string result;
        for (std::size_t i = 0; i < shown; ++i) {
            const Row &row = rows[i];
            if (row.directory) {
                result += "[DIR]  " + row.name + "\n";
            } else {
                result += "[FILE] " + row.name + " (" + std::to_string(row.size) + " bytes)\n";
            }
        }
        if (rows.size() > shown) {
            result += std::to_string(shown) + " of " + std::to_string(rows.size()) + "\n";
        }
        return result;
    } catch (const std::exception & e) {
        return std::string("error listing directory: ") + e.what();
    }
}

Tool create_list_directory_tool() {
    return {
        .name = "list_directory",
        .description = "List files and subdirectories in a directory path.",
        .schema_doc = "arguments:\n      path: string (optional directory path, defaults to '.')",
        .execute = list_directory,
        .present = [](const nlohmann::json &args) { return tool_arg(args, "path"); }
    };
}

} // namespace Tools
