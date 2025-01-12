#pragma once

#include "socket.hpp"
#include "tcp_client_socket.hpp"

namespace network
{
    class tcp_server_socket : public socket
    {
      public:
        tcp_server_socket(int af);

        tcp_server_socket() = default;
        ~tcp_server_socket() override = default;

        tcp_server_socket(tcp_server_socket&& obj) noexcept = default;
        tcp_server_socket& operator=(tcp_server_socket&& obj) noexcept = default;

        tcp_client_socket accept();

      private:
        bool listening_{false};

        void listen();
    };
}
