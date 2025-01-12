#pragma once

#include "socket.hpp"

#include <string_view>

namespace network
{
    struct udp_socket : socket
    {
        udp_socket(int af);
        udp_socket() = default;
        ~udp_socket() override = default;

        udp_socket(udp_socket&& obj) noexcept = default;
        udp_socket& operator=(udp_socket&& obj) noexcept = default;

        [[maybe_unused]] bool send(const address& target, const void* data, size_t size) const;
        [[maybe_unused]] bool send(const address& target, std::string_view data) const;
        std::optional<std::pair<address, std::string>> receive() const;
    };
}
