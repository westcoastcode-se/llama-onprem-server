#pragma once

#include "../../api/models.hpp"
#include "../agent/response_parse.hpp"

/**
 * Internal generation payload submitted to Jobs (built from a Session).
 * Not exposed as a public REST body — use POST /v1/sessions/:id/messages instead.
 */
struct MessagesRequest
{
    std::string system;
    std::vector<ChatMessage> messages;
    std::string session_id;
    int max_tokens = -1;
    // Negative keeps LlamaConfig::temperature.
    float temperature = -1.0f;
    std::vector<ChatTool> tools;
};
