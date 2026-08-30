#include "common/tools/tool_web_search.hpp"
#include "common/tools/web_utils.hpp"
#include <string>

namespace Tools {

std::string web_search(const nlohmann::json & args) {
    std::string query;
    if (args.contains("query") && args["query"].is_string()) {
        query = args["query"].get<std::string>();
    } else if (args.contains("q") && args["q"].is_string()) {
        query = args["q"].get<std::string>();
    } else if (args.contains("search_query") && args["search_query"].is_string()) {
        query = args["search_query"].get<std::string>();
    } else if (args.contains("input") && args["input"].is_string()) {
        query = args["input"].get<std::string>();
    }

    if (query.empty()) {
        return "error: missing required string argument 'query'";
    }

    int limit = 5;
    if (args.contains("limit") && args["limit"].is_number()) {
        limit = args["limit"].get<int>();
    } else if (args.contains("count") && args["count"].is_number()) {
        limit = args["count"].get<int>();
    } else if (args.contains("max_results") && args["max_results"].is_number()) {
        limit = args["max_results"].get<int>();
    }
    if (limit < 1) limit = 1;
    if (limit > 20) limit = 20;

    // Build URL for SearXNG JSON API
    std::string url = "http://localhost:4488/search?q=" + WebUtils::url_encode(query) + "&format=json";

    if (args.contains("categories") && args["categories"].is_string()) {
        url += "&categories=" + WebUtils::url_encode(args["categories"].get<std::string>());
    }
    if (args.contains("language") && args["language"].is_string()) {
        url += "&language=" + WebUtils::url_encode(args["language"].get<std::string>());
    }
    if (args.contains("time_range") && args["time_range"].is_string()) {
        url += "&time_range=" + WebUtils::url_encode(args["time_range"].get<std::string>());
    }

    std::string body;
    std::string error;
    long status = 0;
    if (!WebUtils::http_get(url, body, status, error)) {
        return "error: search request failed: " + error + " (url: " + url + ")";
    }

    if (status != 200) {
        return "error: SearXNG returned HTTP status " + std::to_string(status) + ": " + body;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception & e) {
        return std::string("error: failed to parse SearXNG JSON response: ") + e.what();
    }

    std::string result;

    // 1. Parse direct answers if present
    if (j.contains("answers") && j["answers"].is_array() && !j["answers"].empty()) {
        result += "Answers:\n";
        for (const auto & ans : j["answers"]) {
            if (ans.is_string()) {
                result += "- " + ans.get<std::string>() + "\n";
            } else if (ans.is_object() && ans.contains("answer")) {
                result += "- " + ans["answer"].get<std::string>() + "\n";
            }
        }
        result += "\n";
    }

    // 2. Parse infoboxes if present
    if (j.contains("infoboxes") && j["infoboxes"].is_array() && !j["infoboxes"].empty()) {
        for (const auto & info : j["infoboxes"]) {
            if (info.is_object()) {
                std::string infobox_title = info.value("infobox", "");
                std::string infobox_content = info.value("content", "");
                if (!infobox_title.empty() || !infobox_content.empty()) {
                    result += "Infobox: " + infobox_title + "\n" + infobox_content + "\n\n";
                }
            }
        }
    }

    // 3. Parse search results
    if (j.contains("results") && j["results"].is_array()) {
        const auto & results = j["results"];
        if (results.empty()) {
            return "No search results found for query: \"" + query + "\"";
        }
        int count = 0;
        for (const auto & item : results) {
            if (!item.is_object()) continue;
            count++;
            if (count > limit) break;

            std::string title = item.value("title", "");
            std::string item_url = item.value("url", "");
            std::string content = item.value("content", "");

            result += std::to_string(count) + ". " + title + "\n";
            if (!item_url.empty()) {
                result += "   URL: " + item_url + "\n";
            }
            if (!content.empty()) {
                result += "   Snippet: " + content + "\n";
            }
            result += "\n";
        }
    } else {
        return "No search results found for query: \"" + query + "\"";
    }

    // 4. Parse search suggestions
    if (j.contains("suggestions") && j["suggestions"].is_array() && !j["suggestions"].empty()) {
        result += "Suggestions: ";
        bool first = true;
        for (const auto & sug : j["suggestions"]) {
            if (sug.is_string()) {
                if (!first) result += ", ";
                result += sug.get<std::string>();
                first = false;
            }
        }
        result += "\n";
    }

    if (result.size() > MAX_TOOL_OUTPUT_CHARS) {
        result.resize(MAX_TOOL_OUTPUT_CHARS);
        result += "\n\n[output truncated]";
    }

    return result;
}

Tool create_web_search_tool() {
    return {
        "web_search",
        "Search the internet for queries, web pages, and information.",
        "arguments:\n      query: string (search keywords or question)\n      limit: integer (optional maximum results to return, default 5)",
        web_search,
        {"searxng_search", "searx_search", "internet_search", "search"}
    };
}

} // namespace Tools
