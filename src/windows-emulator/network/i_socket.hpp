#pragma once

#include <span>
#include <network/socket.hpp>

namespace network
{
    struct i_socket
    {
        virtual ~i_socket() = default;

        virtual void set_blocking(bool blocking) = 0;

        virtual int get_last_error() = 0;

        virtual bool is_ready(bool in_poll) = 0;

        virtual bool bind(const address& addr) = 0;

        virtual sent_size send(std::span<const std::byte> data) = 0;
        virtual sent_size sendto(const address& destination, std::span<const std::byte> data) = 0;

        virtual sent_size recv(std::span<std::byte> data) = 0;
        virtual sent_size recvfrom(address& source, std::span<std::byte> data) = 0;
    };
}
