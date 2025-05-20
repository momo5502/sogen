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
        return this->socket_.is_listening();
    }

    bool socket_wrapper::bind(const address& addr)
    {
        return this->socket_.bind(addr);
    }

    bool socket_wrapper::listen(int backlog)
    {
        return this->socket_.listen(backlog);
    }

    std::unique_ptr<i_socket> socket_wrapper::accept(address& address)
    {
        const auto s = this->socket_.accept(address);
        if (s == INVALID_SOCKET)
        {
            return nullptr;
        }

        return std::make_unique<socket_wrapper>(s);
    }

    sent_size socket_wrapper::send(const std::span<const std::byte> data)
    {
        return ::send(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()),
                      static_cast<send_size>(data.size()), 0);
    }

    sent_size socket_wrapper::sendto(const address& destination, const std::span<const std::byte> data)
    {
        return ::sendto(this->socket_.get_socket(), reinterpret_cast<const char*>(data.data()),
                        static_cast<send_size>(data.size()), 0, &destination.get_addr(), destination.get_size());
    }

    sent_size socket_wrapper::recv(std::span<std::byte> data)
    {
        return ::recv(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()),
                      static_cast<send_size>(data.size()), 0);
    }

    sent_size socket_wrapper::recvfrom(address& source, std::span<std::byte> data)
    {
        auto source_length = source.get_max_size();
        const auto res = ::recvfrom(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()),
                                    static_cast<send_size>(data.size()), 0, &source.get_addr(), &source_length);

        assert(source.get_size() == source_length);

        return res;
    }
}
