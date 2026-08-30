#pragma once

#include <string>
#include <string_view>

namespace Tools::WebUtils {

/**
 * @brief URL-encodes a string so it can be safely included in an HTTP GET query string.
 * 
 * Uses libcurl's curl_easy_escape to convert special characters into %XX format.
 *
 * @param value The string to encode (e.g. search query with spaces or special characters).
 * @return URL-encoded string.
 */
std::string url_encode(std::string_view value);

/**
 * @brief Performs an HTTP GET request to the specified URL using libcurl.
 *
 * Follows redirects (up to 5 redirects), sets realistic timeouts,
 * and captures the HTTP status code as well as any network error messages.
 *
 * @param url Target address (must start with http:// or https://).
 * @param[out] body Buffer where response body is stored upon successful transfer.
 * @param[out] status_code HTTP status code from server (e.g. 200, 404, 500).
 * @param[out] error Error message from curl if request fails completely.
 * @return true if the HTTP request completed technically, false otherwise.
 */
bool http_get(const std::string & url, std::string & body, long & status_code, std::string & error);

/**
 * @brief Converts raw HTML into readable plain text.
 *
 * 1. Strips script and style blocks (<script>, <style>, <noscript>).
 * 2. Removes HTML tags (<...>).
 * 3. Decodes common HTML entities (&amp;, &lt;, &gt;, &quot;, &#39;, &nbsp;, etc.).
 * 4. Normalizes multiple whitespaces and newlines for clean presentation to LLMs.
 *
 * @param html Raw HTML string from a web page.
 * @return Cleaned plain text representation of the web page.
 */
std::string html_to_text(std::string_view html);

} // namespace Tools::WebUtils
