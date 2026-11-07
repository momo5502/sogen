#pragma once

#include "socket_factory.hpp"

namespace sogen
{

    namespace network
    {
        // Egress for WebAssembly builds. A browser cannot open a TCP socket, so live networking is
        // serviced by forwarding complete plaintext HTTP/1.1 requests through fetch(). See
        // emscripten_http_socket_factory.cpp for what that does and does not cover.
        std::unique_ptr<socket_factory> create_emscripten_http_socket_factory();
    }

} // namespace sogen
