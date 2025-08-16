#include "socket_wrapper.hpp"
#include <cassert>

namespace network
{
    socket_wrapper::socket_wrapper(SOCKET s)
        : socket_(s)
    {
    }

    socket_wrapper::socket_wrapper(const int af, const int type, const int protocol)
        : socket_(af, type, protocol)
    {
    }

    void socket_wrapper::set_blocking(const bool blocking)
    {
        this->socket_.set_blocking(blocking);
    }

    int socket_wrapper::get_last_error()
    {
        return GET_SOCKET_ERROR();
    }

    bool socket_wrapper::is_ready(const bool in_poll)
    {
        return this->socket_.is_ready(in_poll);
    }

    bool socket_wrapper::is_listening()
    {
        if (!this->socket_.is_valid())
        {
            return false;
        }

        int val{};
        socklen_t len = sizeof(val);
        const auto res = getsockopt(this->socket_.get_socket(), SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&val), &len);

        return res != SOCKET_ERROR && val == 1;
    }

    std::optional<address> socket_wrapper::get_local_address()
    {
        sockaddr addr{};
        socklen_t addrlen = sizeof(sockaddr);
        const auto res = ::getsockname(this->socket_.get_socket(), &addr, &addrlen);

        if (res != 0)
        {
            return {};
        }

        address address{};
        address.set_address(&addr, addrlen);
        return address;
    }

    bool socket_wrapper::bind(const address& addr)
    {
        return this->socket_.bind(addr);
    }

    bool socket_wrapper::connect(const address& addr)
    {
        return ::connect(this->socket_.get_socket(), &addr.get_addr(), addr.get_size()) == 0;
    }

    bool socket_wrapper::listen(int backlog)
    {
        return ::listen(this->socket_.get_socket(), backlog) == 0;
    }

    std::unique_ptr<i_socket> socket_wrapper::accept(address& address)
    {
        sockaddr addr{};
        socklen_t addrlen = sizeof(sockaddr);
        const auto s = ::accept(this->socket_.get_socket(), &addr, &addrlen);

        if (s == INVALID_SOCKET)
        {
            return nullptr;
        }

        address.set_address(&addr, addrlen);

        return std::make_unique<socket_wrapper>(s);
    }

    sent_size socket_wrapper::send(const std::span<const std::byte> data)
    {
        return ::send(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0);
    }

    sent_size socket_wrapper::sendto(const address& destination, const std::span<const std::byte> data)
    {
        return ::sendto(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()), 0,
                        &destination.get_addr(), destination.get_size());
    }

    sent_size socket_wrapper::recv(std::span<std::byte> data)
    {
        return ::recv(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()), static_cast<send_size>(data.size()), 0);
    }

    sent_size socket_wrapper::recvfrom(address& source, std::span<std::byte> data)
    {
        auto source_length = source.get_max_size();
        const auto res = ::recvfrom(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()), static_cast<send_size>(data.size()),
                                    0, &source.get_addr(), &source_length);

        assert(source.get_size() == source_length);

        return res;
    }
}
