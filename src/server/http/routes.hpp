#pragma once

#include "../jobs/jobs.hpp"
#include <httplib.h>

/**
 * State of the application
 */
struct AppState
{
    LlamaEngine &engine;
    Jobs &jobs;
};

/**
 * OpenAI error object. `code` is omitted when empty.
 */
void send_openai_error(httplib::Response &res, int status, std::string_view type, std::string message,
                       std::string_view code = {});

/**
 *
 * @param server The HTTP server
 * @param state Application state
 */
void register_endpoints(httplib::Server &server, AppState &state);
