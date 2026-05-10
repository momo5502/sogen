#pragma once

#include "socket_factory.hpp"

class logger;

namespace network
{
    std::unique_ptr<socket_factory> create_static_socket_factory();

    void set_static_socket_factory_logger(socket_factory& factory, logger* log);
}
