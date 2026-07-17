#pragma once
#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_mount_point_manager(const device_creation_context& context);

} // namespace sogen
