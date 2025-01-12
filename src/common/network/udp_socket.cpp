#include "udp_socket.hpp"

namespace network
{
    udp_socket::udp_socket(const int af)
        : socket(af, SOCK_DGRAM, IPPROTO_UDP)
    {
    }

    bool udp_socket::send(const address& target, const void* data, const size_t size) const
    {
        return this->send(target, std::string_view(static_cast<const char*>(data), size));
    }

    bool udp_socket::send(const address& target, const std::string_view data) const
    {
        while (true)
        {
            const auto res = sendto(this->get_socket(), data.data(), static_cast<send_size>(data.size()), 0,
                                    &target.get_addr(), target.get_size());

            if (res < 0 && GET_SOCKET_ERROR() == SERR(EWOULDBLOCK))
            {
                this->sleep(std::chrono::milliseconds(10), true);
                continue;
            }

            return static_cast<size_t>(res) == data.size();
        }
    }

    std::optional<std::pair<address, std::string>> udp_socket::receive() const
    {
        char buffer[0x2000];
        address source{};
        auto len = source.get_max_size();

        const auto result =
            recvfrom(this->get_socket(), buffer, static_cast<int>(sizeof(buffer)), 0, &source.get_addr(), &len);
        if (result == SOCKET_ERROR)
        {
            return std::nullopt;
        }

        return {{source, std::string(buffer, result)}};
    }
}
