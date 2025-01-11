#include "tcp_server_socket.hpp"

namespace network
{
    tcp_server_socket::tcp_server_socket(const int af)
        : socket(af, SOCK_STREAM, IPPROTO_TCP)
    {
    }

    tcp_client_socket tcp_server_socket::accept()
    {
        this->listen();

        address a{};
        auto len = a.get_max_size();
        auto s = ::accept(this->get_socket(), &a.get_addr(), &len);

        if (s == INVALID_SOCKET)
        {
            return {};
        }

        return tcp_client_socket{s, a};
    }

    void tcp_server_socket::listen()
    {
        if (this->listening_)
        {
            return;
        }

        this->listening_ = ::listen(this->get_socket(), 32) == 0;
    }
}
