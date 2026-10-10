#pragma once

#include "../port.hpp"

namespace sogen
{

    std::unique_ptr<port> create_sspi_rpc_port();

} // namespace sogen
