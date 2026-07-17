#pragma once
#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_afd_endpoint(const device_creation_context& context);
    std::unique_ptr<io_device> create_afd_async_connect_hlp(const device_creation_context& context);

} // namespace sogen
