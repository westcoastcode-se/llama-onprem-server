#pragma once

#include "common/tools/tool_types.hpp"

namespace Tools {

/**
 * @brief Creates and returns the tool definition for "web_fetch".
 *
 * Fetches the content of a web page via HTTP or HTTPS.
 * By default, HTML is automatically converted into clean, readable text
 * (scripts, styles, and tags are removed). Raw HTML can be requested if needed.
 * Output is truncated at MAX_TOOL_OUTPUT_CHARS.
 *
 * JSON Parameters:
 *   - url (string, required): The full web address (http:// or https://).
 *   - type (string, optional, defaults to "text"): "text" for cleaned text or "html" for raw HTML.
 *
 * Return value:
 *   - HTTP status code followed by web page content or an error message.
 */
Tool create_web_fetch_tool();

/**
 * @brief Direct execution function for "web_fetch".
 * @param args JSON object with fields "url" and optional "type".
 * @return String with fetched content or error description.
 */
std::string web_fetch(const nlohmann::json & args);

} // namespace Tools
