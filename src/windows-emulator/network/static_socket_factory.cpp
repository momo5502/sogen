#include "static_socket_factory.hpp"
#include "../logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include <network/socket.hpp>

#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif
#ifndef POLLHUP
#define POLLHUP 0x0010
#endif

namespace network
{
    namespace
    {
        struct pipe_state
        {
            std::deque<std::byte> client_to_server;
            std::deque<std::byte> server_to_client;
            bool client_closed{false};
            bool server_closed{false};
        };

        struct pending_connection
        {
            address client_addr;
            std::shared_ptr<pipe_state> p;
        };

        struct shared_state
        {
            using packet_data = std::vector<std::byte>;
            using packet = std::pair<address, packet_data>;
            using packet_queue = std::queue<packet>;
            using packet_mapping = std::unordered_map<address, packet_queue>;
            using listen_mapping = std::unordered_map<address, std::deque<pending_connection>>;

            packet_mapping packets;
            listen_mapping listen_queues;
        };

        struct static_socket_factory_impl : socket_factory
        {
            std::shared_ptr<shared_state> state = std::make_shared<shared_state>();
            uint16_t port{0};
            logger* log{nullptr};

            void log_call(const char* fmt, ...)
            {
                if (!this->log)
                {
                    return;
                }
                va_list args;
                va_start(args, fmt);
                char buffer[512];
#ifdef _MSC_VER
                vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
#else
                vsnprintf(buffer, sizeof(buffer), fmt, args);
#endif
                va_end(args);
                this->log->print(color::cyan, std::string_view{buffer});
            }

            struct static_socket : i_socket
            {
                static_socket_factory_impl* factory{};
                int error{0};
                address a{};
                address peer_addr{};
                std::shared_ptr<pipe_state> pipe{};
                bool listening{false};
                bool connected{false};
                bool is_server_side{false};
                int backlog{0};

                explicit static_socket(static_socket_factory_impl& f)
                    : factory(&f)
                {
                }

                static_socket(static_socket_factory_impl& f, const int af)
                    : factory(&f)
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

                void log_op(const char* op, const std::string& detail = {})
                {
                    if (!this->factory)
                    {
                        return;
                    }
                    this->factory->log_call("[static_socket] %s local=%s%s%s\n", op, this->a.to_string().c_str(),
                                            detail.empty() ? "" : " ", detail.c_str());
                }

                void set_blocking(const bool blocking) override
                {
                    this->log_op("set_blocking", blocking ? "blocking=true" : "blocking=false");
                    if (blocking)
                    {
                        throw std::runtime_error("Blocking sockets not supported yet!");
                    }
                }

                int get_last_error() override
                {
                    return this->error;
                }

                bool is_ready(const bool in_poll) override
                {
                    if (this->listening)
                    {
                        auto it = this->factory->state->listen_queues.find(this->a);
                        return it != this->factory->state->listen_queues.end() && !it->second.empty();
                    }
                    if (this->pipe && in_poll)
                    {
                        auto& q = this->is_server_side ? this->pipe->client_to_server : this->pipe->server_to_client;
                        bool peer_closed = this->is_server_side ? this->pipe->client_closed : this->pipe->server_closed;
                        return !q.empty() || peer_closed;
                    }
                    return true;
                }

                bool is_listening() override
                {
                    return this->listening;
                }

                std::optional<address> get_local_address() override
                {
                    return this->a;
                }

                bool bind(const address& addr) override
                {
                    this->log_op("bind", "addr=" + addr.to_string());
                    this->a = addr;
                    return true;
                }

                bool connect(const address& addr) override
                {
                    this->log_op("connect", "peer=" + addr.to_string());
                    this->peer_addr = addr;
                    this->error = 0;

                    auto& queues = this->factory->state->listen_queues;
                    auto it = queues.find(addr);
                    if (it != queues.end())
                    {
                        this->pipe = std::make_shared<pipe_state>();
                        this->is_server_side = false;
                        this->connected = true;
                        it->second.push_back({this->a, this->pipe});
                    }
                    else
                    {
                        this->connected = true;
                    }
                    return true;
                }

                bool listen(int backlog_value) override
                {
                    this->log_op("listen", "backlog=" + std::to_string(backlog_value));
                    this->listening = true;
                    this->backlog = backlog_value;
                    this->factory->state->listen_queues[this->a];
                    this->error = 0;
                    return true;
                }

                std::unique_ptr<i_socket> accept(address& out_addr) override
                {
                    this->log_op("accept");
                    auto& queues = this->factory->state->listen_queues;
                    auto it = queues.find(this->a);
                    if (it == queues.end() || it->second.empty())
                    {
                        this->error = SERR(EWOULDBLOCK);
                        return nullptr;
                    }

                    auto pending = std::move(it->second.front());
                    it->second.pop_front();

                    auto sock = std::unique_ptr<static_socket>(new static_socket(*this->factory));
                    sock->a = this->a;
                    sock->peer_addr = pending.client_addr;
                    sock->pipe = pending.p;
                    sock->is_server_side = true;
                    sock->connected = true;

                    out_addr = pending.client_addr;
                    this->error = 0;
                    return sock;
                }

                sent_size send(std::span<const std::byte> data) override
                {
                    this->log_op("send", "len=" + std::to_string(data.size()));
                    if (!this->pipe)
                    {
                        this->error = SERR(ENOTCONN);
                        return -1;
                    }
                    auto& q = this->is_server_side ? this->pipe->server_to_client : this->pipe->client_to_server;
                    q.insert(q.end(), data.begin(), data.end());
                    this->error = 0;
                    return static_cast<sent_size>(data.size());
                }

                sent_size sendto(const address& destination, std::span<const std::byte> data) override
                {
                    this->log_op("sendto", "dest=" + destination.to_string() + " len=" + std::to_string(data.size()));
                    this->error = 0;
                    this->factory->state->packets[destination].emplace(
                        this->a, shared_state::packet_data{data.begin(), data.end()});
                    return static_cast<sent_size>(data.size());
                }

                sent_size recv(std::span<std::byte> data) override
                {
                    this->log_op("recv", "buf=" + std::to_string(data.size()));
                    if (!this->pipe)
                    {
                        this->error = SERR(ENOTCONN);
                        return -1;
                    }
                    auto& q = this->is_server_side ? this->pipe->client_to_server : this->pipe->server_to_client;
                    bool peer_closed = this->is_server_side ? this->pipe->client_closed : this->pipe->server_closed;

                    if (q.empty())
                    {
                        if (peer_closed)
                        {
                            this->error = 0;
                            return 0;
                        }
                        this->error = SERR(EWOULDBLOCK);
                        return -1;
                    }

                    const size_t n = std::min(data.size(), q.size());
                    for (size_t i = 0; i < n; ++i)
                    {
                        data[i] = q.front();
                        q.pop_front();
                    }
                    this->error = 0;
                    return static_cast<sent_size>(n);
                }

                sent_size recvfrom(address& source, std::span<std::byte> data) override
                {
                    this->log_op("recvfrom", "buf=" + std::to_string(data.size()));
                    this->error = 0;

                    auto& q = this->factory->state->packets[this->a];

                    if (q.empty())
                    {
                        this->error = SERR(EWOULDBLOCK);
                        return -1;
                    }

                    const auto p = std::move(q.front());
                    q.pop();

                    memcpy(data.data(), p.second.data(), std::min(data.size(), p.second.size()));

                    source = p.first;
                    return static_cast<sent_size>(p.second.size());
                }
            };

            std::unique_ptr<i_socket> create_socket(const int af, const int type, const int protocol) override
            {
                this->log_call("[static_socket_factory] create_socket af=%d type=%d protocol=%d\n", af, type, protocol);
                return std::make_unique<static_socket>(*this, af);
            }

            int poll_sockets(std::span<poll_entry> entries) override
            {
                this->log_call("[static_socket_factory] poll_sockets count=%zu\n", entries.size());

                int ready_count = 0;
                for (auto& entry : entries)
                {
                    entry.revents = 0;
                    if (!entry.s)
                    {
                        continue;
                    }

                    auto* s = dynamic_cast<static_socket*>(entry.s);
                    if (!s)
                    {
                        continue;
                    }

                    int16_t revents = 0;

                    if (entry.events & POLLIN)
                    {
                        if (s->listening)
                        {
                            auto it = this->state->listen_queues.find(s->a);
                            if (it != this->state->listen_queues.end() && !it->second.empty())
                            {
                                revents |= POLLIN;
                            }
                        }
                        else if (s->pipe)
                        {
                            auto& q = s->is_server_side ? s->pipe->client_to_server : s->pipe->server_to_client;
                            bool peer_closed = s->is_server_side ? s->pipe->client_closed : s->pipe->server_closed;
                            if (!q.empty())
                            {
                                revents |= POLLIN;
                            }
                            if (peer_closed)
                            {
                                revents |= POLLHUP;
                            }
                        }
                        else
                        {
                            auto& q = this->state->packets[s->a];
                            if (!q.empty())
                            {
                                revents |= POLLIN;
                            }
                        }
                    }

                    if (entry.events & POLLOUT)
                    {
                        revents |= POLLOUT;
                    }

                    entry.revents = revents;
                    if (revents != 0)
                    {
                        ++ready_count;
                    }
                }

                return ready_count;
            }
        };
    }

    std::unique_ptr<socket_factory> create_static_socket_factory()
    {
        return std::make_unique<static_socket_factory_impl>();
    }

    void set_static_socket_factory_logger(socket_factory& factory, logger* log)
    {
        if (auto* impl = dynamic_cast<static_socket_factory_impl*>(&factory))
        {
            impl->log = log;
        }
    }
}
