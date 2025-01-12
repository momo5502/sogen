#include "udp_socket.hpp"

namespace network
{
    udp_socket::udp_socket(const int af)
        : socket(af, SOCK_DGRAM, IPPROTO_UDP)
    {
    }

    bool udp_socket::send(const address& target, const void* data, const size_t size) const
    {
        const auto res = sendto(this->get_socket(), static_cast<const char*>(data), static_cast<send_size>(size), 0,
                                &target.get_addr(), target.get_size());
        return static_cast<size_t>(res) == size;
    }

    bool udp_socket::send(const address& target, const std::string_view data) const
    {
        return this->send(target, data.data(), data.size());
    }

    bool udp_socket::receive(address& source, std::string& data) const
    {
        char buffer[0x2000];
        auto len = source.get_max_size();

        const auto result =
            recvfrom(this->get_socket(), buffer, static_cast<int>(sizeof(buffer)), 0, &source.get_addr(), &len);
        if (result == SOCKET_ERROR)
        {
            return false;
        }

        data.assign(buffer, buffer + result);
        return true;
    }
}
