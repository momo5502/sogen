#include "static_socket_factory.hpp"

#include <queue>
#include <stdexcept>
#include <unordered_map>

#include <network/socket.hpp>

namespace network
{
    namespace
    {
        struct static_socket_factory : socket_factory
        {
            using packet_data = std::vector<std::byte>;
            using packet = std::pair<address, packet_data>;
            using packet_queue = std::queue<packet>;
            using packet_mapping = std::unordered_map<address, packet_queue>;
            std::shared_ptr<packet_mapping> packets = std::make_shared<packet_mapping>();

            uint16_t port{0};

            struct static_socket : i_socket
            {
                int error{0};
                address a{};
                std::shared_ptr<packet_mapping> packets{};

                static_socket(static_socket_factory& f, const int af)
                    : packets(f.packets)
                {
                    if (af == AF_INET)
                    {
                        a.set_ipv4(0);
                    }
                    else if (af == AF_INET6)
                    {
                        a.set_ipv6({});
                    }
                    else
                    {
                        throw std::runtime_error("Invalid address family");
                    }

                    a.set_port(++f.port);
                }

                ~static_socket() override = default;

                void set_blocking(const bool blocking) override
                {
                    if (blocking)
                    {
                        throw std::runtime_error("Blocking sockets not supported yet!");
                    }
                }

                int get_last_error() override
                {
                    return this->error;
                }

                bool is_ready(const bool) override
                {
                    return true;
                }

                bool is_listening() override
                {
                    return false;
                }

                std::optional<address> get_local_address() override
                {
                    return this->a;
                }

                bool bind(const address& addr) override
                {
                    this->a = addr;
                    return true;
                }

                bool connect(const address& addr) override
                {
                    this->a = addr;
                    return true;
                }

                bool listen(int) override
                {
                    throw std::runtime_error("Not implemented");
                }

                std::unique_ptr<i_socket> accept(address&) override
                {
                    throw std::runtime_error("Not implemented");
                }

                sent_size send(std::span<const std::byte>) override
                {
                    throw std::runtime_error("Not implemented");
                }

                sent_size sendto(const address& destination, std::span<const std::byte> data) override
                {
                    this->error = 0;
                    (*this->packets)[destination].emplace(this->a, packet_data{data.begin(), data.end()});
                    return static_cast<int>(data.size());
                }

                sent_size recv(std::span<std::byte>) override
                {
                    throw std::runtime_error("Not implemented");
                }

                sent_size recvfrom(address& source, std::span<std::byte> data) override
                {
                    this->error = 0;

                    auto& q = (*this->packets)[this->a];

                    if (q.empty())
                    {
                        this->error = SERR(EWOULDBLOCK);
                        return -1;
                    }

                    const auto p = std::move(q.front());
                    q.pop();

                    memcpy(data.data(), p.second.data(), std::min(data.size(), p.second.size()));

                    source = p.first;
                    return static_cast<int>(p.second.size());
                }
            };

            std::unique_ptr<i_socket> create_socket(const int af, const int, const int) override
            {
                return std::make_unique<static_socket>(*this, af);
            }

            int poll_sockets(std::span<poll_entry>) override
            {
                throw std::runtime_error("Not implemented");
            }
        };
    }

    std::unique_ptr<socket_factory> create_static_socket_factory()
    {
        return std::make_unique<static_socket_factory>();
    }
}
