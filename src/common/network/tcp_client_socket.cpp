#include "tcp_client_socket.hpp"

namespace network
{
    tcp_client_socket::tcp_client_socket(SOCKET s, const address& target)
        : socket(s),
          target_(target)
    {
    }

    bool tcp_client_socket::send(const void* data, const size_t size) const
    {
        const auto res = ::send(this->get_socket(), static_cast<const char*>(data), static_cast<send_size>(size), 0);
        return static_cast<size_t>(res) == size;
    }

    bool tcp_client_socket::send(const std::string_view data) const
    {
        return this->send(data.data(), data.size());
    }

    bool tcp_client_socket::receive(std::string& data) const
    {
        char buffer[0x2000];

        const auto result = recv(this->get_socket(), buffer, static_cast<int>(sizeof(buffer)), 0);
        if (result == SOCKET_ERROR)
        {
            return false;
        }

        data.assign(buffer, buffer + result);
        return true;
    }

    address tcp_client_socket::get_target() const
    {
        return this->target_;
    }
}
