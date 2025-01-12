#include "tcp_client_socket.hpp"

#include <cassert>

namespace network
{
    tcp_client_socket::tcp_client_socket(const int af)
        : socket(af, SOCK_STREAM, IPPROTO_TCP)
    {
    }

    tcp_client_socket::tcp_client_socket(SOCKET s, const address& target)
        : socket(s)
    {
        (void)target;
        assert(this->get_target() == target);
    }

    tcp_client_socket::~tcp_client_socket()
    {
        if (*this && this->get_target())
        {
            ::shutdown(this->get_socket(), SHUT_RDWR);
        }
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

    std::optional<address> tcp_client_socket::get_target() const
    {
        address a{};
        auto len = a.get_max_size();
        if (getpeername(this->get_socket(), &a.get_addr(), &len) == SOCKET_ERROR)
        {
            return std::nullopt;
        }

        return a;
    }

    bool tcp_client_socket::connect(const address& target)
    {
        if (::connect(this->get_socket(), &target.get_addr(), target.get_size()) != SOCKET_ERROR)
        {
            return true;
        }

        const auto error = GET_SOCKET_ERROR();
        return error == SOCK_WOULDBLOCK;
    }
}
