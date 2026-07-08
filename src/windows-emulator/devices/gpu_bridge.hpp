#pragma once

#include "../io_device.hpp"

namespace sogen
{
    std::unique_ptr<io_device> create_gpu_bridge(const device_creation_context& context);
}
