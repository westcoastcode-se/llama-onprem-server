#include "common/tools/tool_search_text.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace Tools {

/**
 * @brief Checks if a file appears to be a binary file by inspecting the first 512 bytes.
 * If null bytes (\0) are encountered, the file is treated as binary and skipped.
 */
static bool is_binary_file(const std::filesystem::path & file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) return false;
    char buffer[512];
    ifs.read(buffer, sizeof(buffer));
    std::streamsize bytes_read = ifs.gcount();
    for (std::streamsize i = 0; i < bytes_read; ++i) {
        if (buffer[i] == '\0') {
            return true;
        }
    }
    return false;
}

/**
 * @brief Determines if a directory should be automatically ignored during recursive search.
 */
static bool should_ignore_dir_name(std::string_view name) {
    return name == ".git" || name == ".svn" || name == ".hg" || name == ".idea" ||
           name == "node_modules" || name == "cmake-build-debug" || name == "cmake-build-release";
}

/**
 * @brief Case-insensitive substring search.
 */
static bool icontains(std::string_view haystack, std::string_view needle_lower) {
    if (needle_lower.empty()) return true;
    if (haystack.size() < needle_lower.size()) return false;

    auto it = std::search(haystack.begin(), haystack.end(),
                          needle_lower.begin(), needle_lower.end(),
                          [](char ch1, char ch2) {
                              return std::tolower(static_cast<unsigned char>(ch1)) == ch2;
                          });
    return it != haystack.end();
}

std::string search_text(const nlohmann::json & args) {
    // 1. Extract query (supports 'query', 'pattern', 'text', 'search')
    std::string query;
    if (args.contains("query") && args["query"].is_string()) {
        query = args["query"].get<std::string>();
    } else if (args.contains("pattern") && args["pattern"].is_string()) {
        query = args["pattern"].get<std::string>();
    } else if (args.contains("text") && args["text"].is_string()) {
        query = args["text"].get<std::string>();
    } else if (args.contains("search") && args["search"].is_string()) {
        query = args["search"].get<std::string>();
    }

    if (query.empty()) {
        return "error: missing required string argument 'query' or 'pattern'";
    }

    // 2. Extract path and filter parameters
    std::string start_path = ".";
    if (args.contains("path") && args["path"].is_string()) {
        start_path = args["path"].get<std::string>();
    }

    std::string file_pattern;
    if (args.contains("file_pattern") && args["file_pattern"].is_string()) {
        file_pattern = args["file_pattern"].get<std::string>();
    } else if (args.contains("extension") && args["extension"].is_string()) {
        file_pattern = args["extension"].get<std::string>();
    } else if (args.contains("include") && args["include"].is_string()) {
        file_pattern = args["include"].get<std::string>();
    }

    bool case_sensitive = false;
    if (args.contains("case_sensitive") && args["case_sensitive"].is_boolean()) {
        case_sensitive = args["case_sensitive"].get<bool>();
    }

    bool is_regex = false;
    if (args.contains("is_regex") && args["is_regex"].is_boolean()) {
        is_regex = args["is_regex"].get<bool>();
    }

    int max_matches = 100;
    if (args.contains("max_matches") && args["max_matches"].is_number()) {
        max_matches = args["max_matches"].get<int>();
    } else if (args.contains("limit") && args["limit"].is_number()) {
        max_matches = args["limit"].get<int>();
    }
    if (max_matches < 1) max_matches = 1;

    // Prepare regex or lowercased query
    std::regex reg_pattern;
    std::string query_lower;
    if (is_regex) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!case_sensitive) {
                flags |= std::regex_constants::icase;
            }
            reg_pattern = std::regex(query, flags);
        } catch (const std::exception & e) {
            return std::string("error: invalid regular expression: ") + e.what();
        }
    } else if (!case_sensitive) {
        query_lower = query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    // Validate start path
    std::error_code ec;
    std::filesystem::path root(start_path);
    if (!std::filesystem::exists(root, ec)) {
        return "error: path '" + start_path + "' does not exist";
    }

    std::string result;
    int match_count = 0;
    bool truncated = false;

    // Helper to search within a single file
    auto search_in_file = [&](const std::filesystem::path & file_path) {
        // Check filename / pattern filter if specified
        if (!file_pattern.empty()) {
            std::string filename = file_path.filename().string();
            std::string path_str = file_path.string();
            if (filename.find(file_pattern) == std::string::npos &&
                path_str.find(file_pattern) == std::string::npos) {
                return;
            }
        }

        // Skip files larger than 5 MB or binary files
        auto fsize = std::filesystem::file_size(file_path, ec);
        if (ec || fsize > 5 * 1024 * 1024 || is_binary_file(file_path)) {
            return;
        }

        std::ifstream file(file_path);
        if (!file.is_open()) return;

        std::string line;
        int line_num = 1;
        while (std::getline(file, line)) {
            bool matches = false;
            if (is_regex) {
                matches = std::regex_search(line, reg_pattern);
            } else if (case_sensitive) {
                matches = (line.find(query) != std::string::npos);
            } else {
                matches = icontains(line, query_lower);
            }

            if (matches) {
                result += file_path.string() + ":" + std::to_string(line_num) + ": " + line + "\n";
                match_count++;
                if (result.size() > MAX_TOOL_OUTPUT_CHARS || match_count >= max_matches) {
                    truncated = true;
                    return;
                }
            }
            line_num++;
        }
    };

    try {
        if (std::filesystem::is_regular_file(root, ec)) {
            search_in_file(root);
        } else if (std::filesystem::is_directory(root, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     root, std::filesystem::directory_options::skip_permission_denied);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                
                // Skip ignored directories
                if (it->is_directory(ec)) {
                    if (should_ignore_dir_name(it->path().filename().string())) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }

                if (it->is_regular_file(ec)) {
                    search_in_file(it->path());
                    if (truncated) {
                        break;
                    }
                }
            }
        }
    } catch (const std::exception & e) {
        return std::string("error searching text: ") + e.what();
    }

    if (result.empty()) {
        return "no matches found for query '" + query + "'";
    }
    if (truncated) {
        result += "\n... [matches truncated]";
    }
    return result;
}

Tool create_search_text_tool() {
    return {
        "search_text",
        "Search for text or regular expressions across files in the project.",
        "arguments:\n      query: string (the text or regex pattern to search for in files)\n      path: string (optional directory or file to search in, default '.')\n      file_pattern: string (optional filename filter or extension, e.g. '.cpp')\n      case_sensitive: boolean (optional, default false)\n      is_regex: boolean (optional, default false)\n      max_matches: integer (optional, default 100)",
        search_text,
        {"grep", "grep_search", "find_text", "search_in_files", "search_files_text", "search_file_content"}
    };
}

} // namespace Tools
