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
        int seen = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 start_path, std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            const std::string filename = it->path().filename().string();
            if (it->is_directory() && is_skipped_directory(filename)) {
                it.disable_recursion_pending();
                continue;
            }
            if (filename.find(pattern) == std::string::npos) {
                continue;
            }
            ++seen;
            if (count >= 100) {
                continue;
            }
            result += it->path().string() + (it->is_directory() ? " [DIR]" : "") + "\n";
            ++count;
        }
        if (seen > count) {
            result += std::to_string(count) + " of " + std::to_string(seen) + "\n";
        }
        return result.empty() ? "no matching files found" : result;
    } catch (const std::exception & e) {
        return std::string("error searching files: ") + e.what();
    }
}

Tool create_file_search_tool() {
    return {
        .name = std::string(kFileSearchName),
        .description = std::string(kFileSearchDescription),
        .schema_doc = std::string(kFileSearchSchema),
        .execute = file_search,
        .present = [](const nlohmann::json &args) { return tool_arg_first(args, {"pattern", "query", "name", "path"}); }
    };
}

} // namespace Tools
