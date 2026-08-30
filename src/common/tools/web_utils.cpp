#include "common/tools/web_utils.hpp"
#include <algorithm>
#include <cctype>
#include <curl/curl.h>

namespace Tools::WebUtils {

/**
 * @brief Callback function for libcurl accumulating downloaded chunks into a std::string.
 */
static size_t http_write_cb(char * ptr, size_t size, size_t nmemb, void * userdata) {
    auto * out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string url_encode(std::string_view value) {
    CURL * curl = curl_easy_init();
    if (!curl) return std::string(value);
    char * encoded = curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
    std::string result = encoded ? encoded : std::string(value);
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return result;
}

bool http_get(const std::string & url, std::string & body, long & status_code, std::string & error) {
    CURL * curl = curl_easy_init();
    if (!curl) {
        error = "failed to initialize curl";
        return false;
    }

    // Configure timeouts and basic parameters for robust fetching
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "local-ai-agent/2.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // Support gzip/deflate
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_cleanup(curl);
    return true;
}

std::string html_to_text(std::string_view html) {
    std::string s(html);

    // Helper to remove contents of script, style and noscript tags
    auto remove_block = [](std::string & str, const std::string & tag) {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string open  = "<" + tag;
        const std::string close = "</" + tag + ">";
        size_t pos = 0;
        while ((pos = lower.find(open, pos)) != std::string::npos) {
            const size_t end = lower.find(close, pos);
            if (end == std::string::npos) {
                str.erase(pos);
                break;
            }
            const size_t stop = end + close.size();
            str.erase(pos, stop - pos);
            lower.erase(pos, stop - pos);
        }
    };
    remove_block(s, "script");
    remove_block(s, "style");
    remove_block(s, "noscript");

    // Clean common HTML tags and retain text
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (const char c : s) {
        if (c == '<') { in_tag = true;  continue; }
        if (c == '>') { in_tag = false; out.push_back(' '); continue; }
        if (!in_tag) out.push_back(c);
    }

    // Replace HTML entities with their corresponding characters
    const std::pair<std::string, std::string> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "}, {"&copy;", "©"},
        {"&mdash;", "—"}, {"&ndash;", "–"}
    };
    for (const auto & [from, to] : entities) {
        size_t pos = 0;
        while ((pos = out.find(from, pos)) != std::string::npos) {
            out.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // Collapse multiple whitespaces and newlines
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    bool prev_newline = false;
    for (const char c : out) {
        if (c == '\n' || c == '\r') {
            if (!prev_newline) collapsed.push_back('\n');
            prev_newline = true;
            prev_space = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) collapsed.push_back(' ');
            prev_space = true;
            continue;
        }
        collapsed.push_back(c);
        prev_space = false;
        prev_newline = false;
    }

    const size_t begin = collapsed.find_first_not_of(" \n");
    return begin == std::string::npos ? std::string() : collapsed.substr(begin);
}

} // namespace Tools::WebUtils
