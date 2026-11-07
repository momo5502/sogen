#include "socket_wrapper.hpp"
#include <cassert>

namespace sogen
{

    namespace network
    {
        namespace
        {
            template <typename Query>
            std::optional<address> query_address(const SOCKET s, const Query query)
            {
                sockaddr_storage addr{};
                socklen_t addrlen = sizeof(sockaddr_storage);
                if (query(s, reinterpret_cast<sockaddr*>(&addr), &addrlen) != 0)
                {
                    return {};
                }

                address result{};
                result.set_address(reinterpret_cast<sockaddr*>(&addr), addrlen);
                return result;
            }
        }

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

        std::optional<int> socket_wrapper::get_socket_error()
        {
            int error{};
            socklen_t length = sizeof(error);
            if (::getsockopt(this->socket_.get_socket(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == SOCKET_ERROR)
            {
                return std::nullopt;
            }

            return error;
        }

        bool socket_wrapper::is_ready(const bool in_poll)
        {
            return this->socket_.is_ready(in_poll);
        }

        bool socket_wrapper::is_listening()
        {
            return this->listening_;
        }

        std::optional<address> socket_wrapper::get_local_address()
        {
            return query_address(this->socket_.get_socket(), ::getsockname);
        }

        std::optional<address> socket_wrapper::get_remote_address()
        {
            return query_address(this->socket_.get_socket(), ::getpeername);
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
            if (::listen(this->socket_.get_socket(), backlog) != 0)
            {
                return false;
            }

            this->listening_ = true;
            return true;
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

        sent_size socket_wrapper::peek(std::span<std::byte> data)
        {
            return ::recv(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()), static_cast<send_size>(data.size()), MSG_PEEK);
        }

        sent_size socket_wrapper::recvfrom(address& source, std::span<std::byte> data)
        {
            auto source_length = source.get_max_size();
            const auto res = ::recvfrom(this->socket_.get_socket(), reinterpret_cast<char*>(data.data()),
                                        static_cast<send_size>(data.size()), 0, &source.get_addr(), &source_length);

            assert(res < 0 || source.get_size() == source_length);

            return res;
        }
    }

} // namespace sogen
