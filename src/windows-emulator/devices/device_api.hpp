#pragma once

#include "../io_device.hpp"

namespace sogen
{
    std::unique_ptr<io_device> create_device_api_device(const device_creation_context& context);
}
