#pragma once
#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_security_support_provider();

} // namespace sogen
