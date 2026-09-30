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
    return is_skipped_directory(name);
}

// method extended: text1|text2 is either alternative, and the pipe itself is not searched for.
static std::vector<std::string> literal_alternatives(std::string_view query)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= query.size())
    {
        const std::size_t bar = query.find('|', start);
        const std::size_t end = bar == std::string_view::npos ? query.size() : bar;
        std::string_view part = query.substr(start, end - start);
        const std::size_t lead = part.find_first_not_of(" \t");
        if (lead != std::string_view::npos)
        {
            const std::size_t tail = part.find_last_not_of(" \t");
            parts.emplace_back(part.substr(lead, tail - lead + 1));
        }
        if (bar == std::string_view::npos)
        {
            break;
        }
        start = bar + 1;
    }
    return parts;
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
                              return std::tolower(static_cast<unsigned char>(ch1)) ==
                                     std::tolower(static_cast<unsigned char>(ch2));
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

    std::string method = "text";
    if (args.contains("method") && args["method"].is_string()) {
        method = args["method"].get<std::string>();
    }
    if (method.empty()) {
        method = "text";
    }
    const bool is_regex = method == "regex";
    const bool is_extended = method == "extended";
    if (!is_regex && !is_extended && method != "text") {
        return "error: method must be text, extended, or regex";
    }

    int max_matches = 100;
    if (args.contains("max_matches") && args["max_matches"].is_number()) {
        max_matches = args["max_matches"].get<int>();
    } else if (args.contains("limit") && args["limit"].is_number()) {
        max_matches = args["limit"].get<int>();
    }
    if (max_matches < 1) max_matches = 1;

    int before = 3;
    if (args.contains("before") && args["before"].is_number()) {
        before = args["before"].get<int>();
    }
    if (before < 0) before = 0;

    int context = 2;
    if (args.contains("context") && args["context"].is_number()) {
        context = args["context"].get<int>();
    }
    if (context < 0) context = 0;
    if (context > 5) context = 5;

    // text is a literal. extended splits on |. regex is an ECMAScript pattern.
    std::regex reg_pattern;
    std::vector<std::string> needles;
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
    } else {
        needles = is_extended ? literal_alternatives(query) : std::vector<std::string>{query};
        if (needles.empty()) {
            return "error: missing required string argument 'query' or 'pattern'";
        }
        if (!case_sensitive) {
            for (std::string & needle : needles) {
                std::transform(needle.begin(), needle.end(), needle.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
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

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(std::move(line));
        }

        std::vector<int> hits;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            bool matches = false;
            if (is_regex) {
                matches = std::regex_search(lines[static_cast<std::size_t>(i)], reg_pattern);
            } else {
                const std::string & line_text = lines[static_cast<std::size_t>(i)];
                for (const std::string & needle : needles) {
                    matches = case_sensitive ? line_text.find(needle) != std::string::npos
                                             : icontains(line_text, needle);
                    if (matches) {
                        break;
                    }
                }
            }
            if (matches) {
                hits.push_back(i);
            }
        }
        if (hits.empty()) return;

        const int room = max_matches - match_count;
        if (static_cast<int>(hits.size()) > room) {
            hits.resize(static_cast<std::size_t>(room));
            truncated = true;
        }
        match_count += static_cast<int>(hits.size());

        std::vector<char> show(lines.size(), 0);
        for (const int hit : hits) {
            const int from = hit > before ? hit - before : 0;
            const int to = std::min(static_cast<int>(lines.size()) - 1, hit + context);
            for (int i = from; i <= to; ++i) {
                show[static_cast<std::size_t>(i)] = 1;
            }
        }
        bool named = false;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            if (show[static_cast<std::size_t>(i)] == 0) continue;
            if (!named) {
                if (!result.empty()) {
                    result += "\n";
                }
                result += file_path.string() + "\n";
                named = true;
            }
            result += std::to_string(i + 1) + ": " + lines[static_cast<std::size_t>(i)] + "\n";
            if (result.size() > MAX_TOOL_OUTPUT_CHARS) {
                truncated = true;
                return;
            }
        }
        if (truncated) return;
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
        .name = std::string(kSearchTextName),
        .description = std::string(kSearchTextDescription),
        .schema_doc = std::string(kSearchTextSchema),
        .execute = search_text,
        .present = [](const nlohmann::json &args) {
            return tool_arg_first(args, {"query", "pattern", "text", "search", "path"});
        },
        .aliases = {"grep", "grep_search", "find_text", "search_in_files", "search_files_text", "search_file_content"}};
}

} // namespace Tools
