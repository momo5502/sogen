#pragma once

#include "socket_factory.hpp"

namespace sogen
{

namespace network
{
    std::unique_ptr<socket_factory> create_static_socket_factory();
}

} // namespace sogen
