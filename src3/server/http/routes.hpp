#pragma once

#include "../jobs/jobs.hpp"
#include "../sessions/sessions.hpp"
#include <httplib.h>

struct AppState
{
    LlamaEngine &engine;
    Jobs &jobs;
    Sessions &sessions;
};

/**
 *
 * @param server The HTT server
 * @param state Application state
 */
void register_endpoints(httplib::Server &server, AppState &state);
