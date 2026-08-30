#include "common/tools/tool_web_fetch.hpp"
#include "common/tools/web_utils.hpp"
#include <string>

namespace Tools {

std::string web_fetch(const nlohmann::json & args) {
    if (!args.contains("url") || !args["url"].is_string()) {
        return "error: missing required string argument 'url'";
    }
    std::string url = args["url"].get<std::string>();
    std::string type = "text";
    if (args.contains("type") && args["type"].is_string()) {
        type = args["type"].get<std::string>();
    }

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return "error: url must start with http:// or https://";
    }

    std::string body;
    std::string error;
    long status = 0;
    if (!WebUtils::http_get(url, body, status, error)) {
        return "error: request failed: " + error;
    }

    std::string text = (type == "html") ? body : WebUtils::html_to_text(body);
    bool truncated = false;
    if (text.size() > MAX_TOOL_OUTPUT_CHARS) {
        text.resize(MAX_TOOL_OUTPUT_CHARS);
        truncated = true;
    }

    std::string result = "HTTP " + std::to_string(status) + " for " + url + "\n\n" + text;
    if (truncated) {
        result += "\n\n[content truncated]";
    }
    return result;
}

Tool create_web_fetch_tool() {
    return {
        "web_fetch",
        "Fetch the readable text or html content of a web page via HTTP(S).",
        "arguments:\n      url: string (absolute http/https url)\n      type: 'text' or 'html' (optional, defaults to 'text')",
        web_fetch
    };
}

} // namespace Tools
