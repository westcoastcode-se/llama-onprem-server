#pragma once

#include <httplib.h>

#include "common/log.hpp"

/**
 * Return json from the server
 *
 * @tparam T The response type
 * @param res A http response
 * @param status Returning response code
 * @param value The value to be returned
 */
template <typename T> void send_json(httplib::Response &res, int status, const T &value)
{
    res.status = status;
    res.set_content(value.to_json().dump(), "application/json");
    log_debug(value.to_json());
}
