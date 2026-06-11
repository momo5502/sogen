#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_core_messaging_registrar_port();

} // namespace sogen
