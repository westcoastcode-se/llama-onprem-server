#pragma once

#include "jobs/jobs.hpp"
#include "sessions/sessions.hpp"
#include <httplib.h>

struct AppState
{
    LlamaEngine &engine;
    Jobs &jobs;
    Sessions &sessions;
};

void register_endpoints(httplib::Server &server, AppState &state);
