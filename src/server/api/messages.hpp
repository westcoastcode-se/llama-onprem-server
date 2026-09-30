#pragma once

#include "../../api/models.hpp"
#include "../agent/response_parse.hpp"

/**
 * Internal generation payload submitted to Jobs (built from a Session).
 * Not exposed as a public REST body — use POST /v1/sessions/:id/messages instead.
 */
struct MessagesRequest
{
    string system;
    vector<ChatMessage> messages;
    string session_id;
    int max_tokens = -1;
    vector<ChatTool> tools;
};
