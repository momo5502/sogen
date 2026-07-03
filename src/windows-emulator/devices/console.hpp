#pragma once

#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_console_device();

} // namespace sogen
