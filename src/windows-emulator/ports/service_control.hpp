#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_service_control_port();

} // namespace sogen
