#pragma once

#include <stdexcept>
#include <string>

struct BadRequest : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
