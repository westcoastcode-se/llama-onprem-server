#pragma once

#include "../../api/models.hpp"
#include "../agent/response_parse.hpp"

/**
 * Internal generation payload submitted to Jobs from POST /v1/responses.
 * Not a public REST body.
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
    // Declared Responses namespaces. Tool names inside them are namespace + "." + member.
    std::vector<std::string> tool_namespaces;
};
