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
        return this->send(std::string_view(static_cast<const char*>(data), size));
    }

    bool tcp_client_socket::send(std::string_view data) const
    {
        while (!data.empty())
        {
            const auto res = ::send(this->get_socket(), data.data(), static_cast<send_size>(data.size()), 0);
            if (res < 0)
            {
                if (GET_SOCKET_ERROR() != SERR(EWOULDBLOCK))
                {
                    break;
                }

                this->sleep(std::chrono::milliseconds(10), true);
                continue;
            }

            if (static_cast<size_t>(res) > data.size())
            {
                break;
            }

            data = data.substr(res);
        }

        return data.empty();
    }

    std::optional<std::string> tcp_client_socket::receive(const std::optional<size_t> max_size)
    {
        char buffer[0x2000];
        const auto size = std::min(sizeof(buffer), max_size.value_or(sizeof(buffer)));

        const auto result = recv(this->get_socket(), buffer, static_cast<int>(size), 0);
        if (result > 0)
        {
            return std::string(buffer, result);
        }

        if (result == 0 || (result < 0 && GET_SOCKET_ERROR() == SERR(ECONNRESET)))
        {
            this->close();
        }

        return std::nullopt;
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
        return error == SERR(EWOULDBLOCK);
    }
}
