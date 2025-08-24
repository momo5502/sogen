#include "../std_include.hpp"
#include "afd_endpoint.hpp"
#include "afd_types.hpp"

#include "../windows_emulator.hpp"
#include "../network/socket_factory.hpp"

#include <network/address.hpp>
#include <network/socket.hpp>

#include <utils/finally.hpp>
#include <utils/time.hpp>

namespace
{
    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

    struct afd_creation_data
    {
        uint64_t unk1;
        char afd_open_packet_xx[0x10];
        uint64_t unk2;
        int address_family;
        int type;
        int protocol;
        // ...
    };

    struct win_sockaddr
    {
        int16_t sa_family;
        uint8_t sa_data[14];
    };

    struct win_sockaddr_in
    {
        int16_t sin_family;
        uint16_t sin_port;
        in_addr sin_addr;
        uint8_t sin_zero[8];
    };

    struct win_sockaddr_in6
    {
        int16_t sin6_family;
        uint16_t sin6_port;
        uint32_t sin6_flowinfo;
        in6_addr sin6_addr;
        uint32_t sin6_scope_id;
    };

    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

    static_assert(sizeof(win_sockaddr) == 16);
    static_assert(sizeof(win_sockaddr_in) == 16);
    static_assert(sizeof(win_sockaddr_in6) == 28);

    static_assert(sizeof(win_sockaddr_in::sin_addr) == 4);
    static_assert(sizeof(win_sockaddr_in6::sin6_addr) == 16);
    static_assert(sizeof(win_sockaddr_in6::sin6_flowinfo) == sizeof(sockaddr_in6::sin6_flowinfo));
    static_assert(sizeof(win_sockaddr_in6::sin6_scope_id) == sizeof(sockaddr_in6::sin6_scope_id));

    const std::map<int, int> address_family_map{
        {0, AF_UNSPEC}, //
        {2, AF_INET},   //
        {23, AF_INET6}, //
    };

    const std::map<int, int> socket_type_map{
        {0, 0},           //
        {1, SOCK_STREAM}, //
        {2, SOCK_DGRAM},  //
        {3, SOCK_RAW},    //
        {4, SOCK_RDM},    //
    };

    const std::map<int, int> socket_protocol_map{
        {0, 0},             //
        {6, IPPROTO_TCP},   //
        {17, IPPROTO_UDP},  //
        {255, IPPROTO_RAW}, //
    };

    int16_t translate_host_to_win_address_family(const int host_af)
    {
        for (const auto& entry : address_family_map)
        {
            if (entry.second == host_af)
            {
                return static_cast<int16_t>(entry.first);
            }
        }

        throw std::runtime_error("Unknown host address family: " + std::to_string(host_af));
    }

    int translate_win_to_host_address_family(const int win_af)
    {
        const auto entry = address_family_map.find(win_af);
        if (entry != address_family_map.end())
        {
            return entry->second;
        }

        throw std::runtime_error("Unknown address family: " + std::to_string(win_af));
    }

    int translate_win_to_host_type(const int win_type)
    {
        const auto entry = socket_type_map.find(win_type);
        if (entry != socket_type_map.end())
        {
            return entry->second;
        }

        throw std::runtime_error("Unknown socket type: " + std::to_string(win_type));
    }

    int translate_win_to_host_protocol(const int win_protocol)
    {
        const auto entry = socket_protocol_map.find(win_protocol);
        if (entry != socket_protocol_map.end())
        {
            return entry->second;
        }

        throw std::runtime_error("Unknown socket protocol: " + std::to_string(win_protocol));
    }

    std::vector<std::byte> convert_to_win_address(const windows_emulator& win_emu, const network::address& a)
    {
        if (a.is_ipv4())
        {
            win_sockaddr_in win_addr{};
            win_addr.sin_family = translate_host_to_win_address_family(a.get_family());
            win_addr.sin_port = htons(win_emu.get_emulator_port(a.get_port()));
            memcpy(&win_addr.sin_addr, &a.get_in_addr().sin_addr, sizeof(win_addr.sin_addr));

            const auto* ptr = reinterpret_cast<std::byte*>(&win_addr);
            return {ptr, ptr + sizeof(win_addr)};
        }

        if (a.is_ipv6())
        {
            win_sockaddr_in6 win_addr{};
            win_addr.sin6_family = translate_host_to_win_address_family(a.get_family());
            win_addr.sin6_port = htons(win_emu.get_emulator_port(a.get_port()));

            const auto& addr = a.get_in6_addr();
            memcpy(&win_addr.sin6_addr, &addr.sin6_addr, sizeof(win_addr.sin6_addr));
            win_addr.sin6_flowinfo = addr.sin6_flowinfo;
            win_addr.sin6_scope_id = addr.sin6_scope_id;

            const auto* ptr = reinterpret_cast<std::byte*>(&win_addr);
            return {ptr, ptr + sizeof(win_addr)};
        }

        throw std::runtime_error("Unsupported host address family for conversion: " + std::to_string(a.get_family()));
    }

    network::address convert_to_host_address(const windows_emulator& win_emu, const std::span<const std::byte> data)
    {
        if (data.size() < sizeof(win_sockaddr))
        {
            throw std::runtime_error("Bad address size");
        }

        win_sockaddr win_addr{};
        memcpy(&win_addr, data.data(), sizeof(win_addr));

        const auto family = translate_win_to_host_address_family(win_addr.sa_family);

        network::address a{};

        if (family == AF_INET)
        {
            if (data.size() < sizeof(win_sockaddr_in))
            {
                throw std::runtime_error("Bad IPv4 address size");
            }

            win_sockaddr_in win_addr4{};
            memcpy(&win_addr4, data.data(), sizeof(win_addr4));

            a.set_ipv4(win_addr4.sin_addr);
            a.set_port(win_emu.get_host_port(ntohs(win_addr4.sin_port)));

            return a;
        }

        if (family == AF_INET6)
        {
            if (data.size() < sizeof(win_sockaddr_in6))
            {
                throw std::runtime_error("Bad IPv6 address size");
            }

            win_sockaddr_in6 win_addr6{};
            memcpy(&win_addr6, data.data(), sizeof(win_addr6));

            a.set_ipv6(win_addr6.sin6_addr);
            a.set_port(ntohs(win_addr6.sin6_port));

            auto& addr = a.get_in6_addr();
            addr.sin6_flowinfo = win_addr6.sin6_flowinfo;
            addr.sin6_scope_id = win_addr6.sin6_scope_id;

            return a;
        }

        throw std::runtime_error("Unsupported win address family for conversion: " + std::to_string(family));
    }

    afd_creation_data get_creation_data(windows_emulator& win_emu, const io_device_creation_data& data)
    {
        if (!data.buffer || data.length < sizeof(afd_creation_data))
        {
            throw std::runtime_error("Bad AFD creation data");
        }

        return win_emu.emu().read_memory<afd_creation_data>(data.buffer);
    }

    std::pair<AFD_POLL_INFO64, std::vector<AFD_POLL_HANDLE_INFO64>> get_poll_info(windows_emulator& win_emu, const io_device_context& c)
    {
        constexpr auto info_size = offsetof(AFD_POLL_INFO64, Handles);
        if (!c.input_buffer || c.input_buffer_length < info_size || c.input_buffer != c.output_buffer)
        {
            throw std::runtime_error("Bad AFD poll data");
        }

        AFD_POLL_INFO64 poll_info{};
        win_emu.emu().read_memory(c.input_buffer, &poll_info, info_size);

        std::vector<AFD_POLL_HANDLE_INFO64> handle_info{};

        const emulator_object<AFD_POLL_HANDLE_INFO64> handle_info_obj{win_emu.emu(), c.input_buffer + info_size};

        if (c.input_buffer_length < (info_size + sizeof(AFD_POLL_HANDLE_INFO64) * poll_info.NumberOfHandles))
        {
            throw std::runtime_error("Bad AFD poll handle data");
        }

        for (ULONG i = 0; i < poll_info.NumberOfHandles; ++i)
        {
            handle_info.emplace_back(handle_info_obj.read(i));
        }

        return {poll_info, std::move(handle_info)};
    }

    int16_t map_afd_request_events_to_socket(const ULONG poll_events)
    {
        int16_t socket_events{};

        if (poll_events & (AFD_POLL_DISCONNECT | AFD_POLL_ACCEPT | AFD_POLL_RECEIVE))
        {
            socket_events |= POLLRDNORM;
        }

        if (poll_events & AFD_POLL_RECEIVE_EXPEDITED)
        {
            socket_events |= POLLRDBAND;
        }

        if (poll_events & (AFD_POLL_CONNECT | AFD_POLL_CONNECT_FAIL | AFD_POLL_SEND))
        {
            socket_events |= POLLWRNORM;
        }

        return socket_events;
    }

    ULONG map_socket_response_events_to_afd(const int16_t socket_events, const ULONG afd_poll_events, const bool is_listening,
                                            const bool is_connecting)
    {
        ULONG afd_events = 0;

        if (socket_events & POLLRDNORM)
        {
            if (!is_listening && afd_poll_events & AFD_POLL_RECEIVE)
            {
                afd_events |= AFD_POLL_RECEIVE;
            }
            else if (is_listening && afd_poll_events & AFD_POLL_ACCEPT)
            {
                afd_events |= AFD_POLL_ACCEPT;
            }
        }

        if (socket_events & POLLRDBAND && afd_poll_events & AFD_POLL_RECEIVE_EXPEDITED)
        {
            afd_events |= AFD_POLL_RECEIVE_EXPEDITED;
        }

        if (socket_events & POLLWRNORM)
        {
            if (!is_connecting && afd_poll_events & AFD_POLL_SEND)
            {
                afd_events |= AFD_POLL_SEND;
            }
            else if (is_connecting && afd_poll_events & AFD_POLL_CONNECT)
            {
                afd_events |= AFD_POLL_CONNECT;
            }
        }

        if ((socket_events & (POLLHUP | POLLERR)) == (POLLHUP | POLLERR) && afd_poll_events & (AFD_POLL_CONNECT_FAIL | AFD_POLL_ABORT))
        {
            afd_events |= (AFD_POLL_CONNECT_FAIL | AFD_POLL_ABORT);
        }
        else if (socket_events & POLLHUP && afd_poll_events & AFD_POLL_DISCONNECT)
        {
            afd_events |= AFD_POLL_DISCONNECT;
        }

        if (socket_events & POLLNVAL && afd_poll_events & AFD_POLL_LOCAL_CLOSE)
        {
            afd_events |= AFD_POLL_LOCAL_CLOSE;
        }

        return afd_events;
    }

    struct afd_endpoint : io_device
    {
        struct pending_connection
        {
            network::address remote_address;
            std::unique_ptr<network::i_socket> accepted_socket;
        };

        std::unique_ptr<network::i_socket> s_{};

        bool executing_delayed_ioctl_{};
        std::optional<afd_creation_data> creation_data{};
        std::optional<bool> require_poll_{};
        std::optional<io_device_context> delayed_ioctl_{};
        std::optional<std::chrono::steady_clock::time_point> timeout_{};
        std::optional<std::function<void(windows_emulator&, const io_device_context&)>> timeout_callback_{};

        std::unordered_map<LONG, pending_connection> pending_connections_{};
        LONG next_sequence_{0};

        std::optional<handle> event_select_event_{};
        ULONG event_select_mask_{0};
        ULONG triggered_events_{0};

        afd_endpoint()
        {
            network::initialize_wsa();
        }

        afd_endpoint(afd_endpoint&&) = delete;
        afd_endpoint& operator=(afd_endpoint&&) = delete;

        ~afd_endpoint() override = default;

        void create(windows_emulator& win_emu, const io_device_creation_data& data) override
        {
            this->creation_data = get_creation_data(win_emu, data);
            this->setup(win_emu.socket_factory());
        }

        void setup(network::socket_factory& factory)
        {
            if (!this->creation_data)
            {
                return;
            }

            const auto& data = *this->creation_data;

            const auto af = translate_win_to_host_address_family(data.address_family);
            const auto type = translate_win_to_host_type(data.type);
            const auto protocol = translate_win_to_host_protocol(data.protocol);

            this->s_ = factory.create_socket(af, type, protocol);
            if (!this->s_)
            {
                throw std::runtime_error("Failed to create socket!");
            }

            this->s_->set_blocking(false);
        }

        void delay_ioctrl(const io_device_context& c, const std::optional<bool> require_poll = {},
                          const std::optional<std::chrono::steady_clock::time_point> timeout = {},
                          const std::optional<std::function<void(windows_emulator&, const io_device_context&)>>& timeout_callback = {})
        {
            if (this->executing_delayed_ioctl_)
            {
                return;
            }

            this->timeout_callback_ = timeout_callback;
            this->timeout_ = timeout;
            this->require_poll_ = require_poll;
            this->delayed_ioctl_ = c;
        }

        void clear_pending_state()
        {
            this->timeout_callback_ = {};
            this->timeout_ = {};
            this->require_poll_ = {};
            this->delayed_ioctl_ = {};
        }

        void work(windows_emulator& win_emu) override
        {
            if (!this->s_ || (!this->delayed_ioctl_ && !this->event_select_mask_))
            {
                return;
            }

            network::poll_entry pfd{};
            pfd.s = this->s_.get();

            if (this->delayed_ioctl_ && this->require_poll_.has_value())
            {
                pfd.events |= *this->require_poll_ ? POLLIN : POLLOUT;
            }
            if (this->event_select_mask_)
            {
                pfd.events = static_cast<int16_t>(pfd.events | map_afd_request_events_to_socket(this->event_select_mask_));
            }
            pfd.revents = pfd.events;

            if (pfd.events != 0)
            {
                win_emu.socket_factory().poll_sockets(std::span{&pfd, 1});
            }

            const auto socket_events = pfd.revents;

            if (socket_events && this->event_select_mask_)
            {
                const bool is_connecting = this->delayed_ioctl_ && _AFD_REQUEST(this->delayed_ioctl_->io_control_code) == AFD_CONNECT;
                ULONG current_events =
                    map_socket_response_events_to_afd(socket_events, this->event_select_mask_, pfd.s->is_listening(), is_connecting);

                if ((current_events & ~this->triggered_events_) != 0)
                {
                    this->triggered_events_ |= current_events;

                    if (auto* event = win_emu.process.events.get(*this->event_select_event_))
                    {
                        event->signaled = true;
                    }
                }
            }

            if (this->delayed_ioctl_)
            {
                this->executing_delayed_ioctl_ = true;
                const auto _ = utils::finally([&] { this->executing_delayed_ioctl_ = false; });

                if (this->require_poll_.has_value())
                {
                    const auto is_ready = socket_events & ((*this->require_poll_ ? POLLIN : POLLOUT) | POLLHUP | POLLERR);
                    if (!is_ready)
                    {
                        return;
                    }
                }

                const auto status = this->execute_ioctl(win_emu, *this->delayed_ioctl_);
                if (status == STATUS_PENDING)
                {
                    if (!this->timeout_ || this->timeout_ > win_emu.clock().steady_now())
                    {
                        return;
                    }

                    write_io_status(this->delayed_ioctl_->io_status_block, STATUS_TIMEOUT);

                    if (this->timeout_callback_)
                    {
                        (*this->timeout_callback_)(win_emu, *this->delayed_ioctl_);
                    }
                }

                auto* e = win_emu.process.events.get(this->delayed_ioctl_->event);
                if (e)
                {
                    e->signaled = true;
                }

                this->clear_pending_state();
            }
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read_optional(this->creation_data);
            this->setup(buffer.read<socket_factory_wrapper>());

            buffer.read_optional(this->require_poll_);
            buffer.read_optional(this->delayed_ioctl_);
            buffer.read_optional(this->timeout_);
        }

        void serialize_object(utils::buffer_serializer& buffer) const override
        {
            buffer.write_optional(this->creation_data);
            buffer.write_optional(this->require_poll_);
            buffer.write_optional(this->delayed_ioctl_);
            buffer.write_optional(this->timeout_);
        }

        NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
        {
            if (_AFD_BASE(c.io_control_code) != FSCTL_AFD_BASE)
            {
                win_emu.log.error("Bad AFD IOCTL: 0x%X\n", static_cast<uint32_t>(c.io_control_code));
                return STATUS_NOT_SUPPORTED;
            }

            const auto request = _AFD_REQUEST(c.io_control_code);

            switch (request)
            {
            case AFD_BIND:
                return this->ioctl_bind(win_emu, c);
            case AFD_CONNECT:
                return this->ioctl_connect(win_emu, c);
            case AFD_START_LISTEN:
                return this->ioctl_listen(win_emu, c);
            case AFD_WAIT_FOR_LISTEN:
                return this->ioctl_wait_for_listen(win_emu, c);
            case AFD_ACCEPT:
                return this->ioctl_accept(win_emu, c);
            case AFD_SEND:
                return this->ioctl_send(win_emu, c);
            case AFD_RECEIVE:
                return this->ioctl_receive(win_emu, c);
            case AFD_SEND_DATAGRAM:
                return this->ioctl_send_datagram(win_emu, c);
            case AFD_RECEIVE_DATAGRAM:
                return this->ioctl_receive_datagram(win_emu, c);
            case AFD_POLL:
                return this->ioctl_poll(win_emu, c);
            case AFD_GET_ADDRESS:
                return this->ioctl_get_address(win_emu, c);
            case AFD_EVENT_SELECT:
                return this->ioctl_event_select(win_emu, c);
            case AFD_ENUM_NETWORK_EVENTS:
                return this->ioctl_enum_network_events(win_emu, c);
            case AFD_SET_CONTEXT:
            case AFD_GET_INFORMATION:
            case AFD_SET_INFORMATION:
            case AFD_QUERY_HANDLES:
            case AFD_TRANSPORT_IOCTL:
                return STATUS_SUCCESS;
            default:
                win_emu.log.error("Unsupported AFD IOCTL: 0x%X (%u)\n", static_cast<uint32_t>(c.io_control_code),
                                  static_cast<uint32_t>(request));
                return STATUS_NOT_SUPPORTED;
            }
        }

        NTSTATUS ioctl_connect(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            auto data = win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);

            constexpr auto address_offset = 24;

            if (data.size() < address_offset)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto addr = convert_to_host_address(win_emu, std::span(data).subspan(address_offset));

            if (!this->s_->connect(addr))
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, false);
                    return STATUS_PENDING;
                }

                if (this->executing_delayed_ioctl_ && error == SERR(EISCONN))
                {
                    return STATUS_SUCCESS;
                }

                return STATUS_UNSUCCESSFUL;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_bind(windows_emulator& win_emu, const io_device_context& c) const
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            auto data = win_emu.emu().read_memory(c.input_buffer, c.input_buffer_length);

            constexpr auto address_offset = 4;

            if (data.size() < address_offset)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto addr = convert_to_host_address(win_emu, std::span(data).subspan(address_offset));

            if (!this->s_->bind(addr))
            {
                return STATUS_ADDRESS_ALREADY_ASSOCIATED;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_listen(windows_emulator& win_emu, const io_device_context& c) const
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            if (c.input_buffer_length < sizeof(AFD_LISTEN_INFO))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto listen_info = win_emu.emu().read_memory<AFD_LISTEN_INFO>(c.input_buffer);

            if (!this->s_->listen(static_cast<int>(listen_info.MaximumConnectionQueue)))
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_wait_for_listen(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            if (c.output_buffer_length < sizeof(AFD_LISTEN_RESPONSE_INFO))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            network::address remote_address{};
            auto accepted_socket_ptr = this->s_->accept(remote_address);

            if (!accepted_socket_ptr)
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, true);
                    return STATUS_PENDING;
                }

                return STATUS_UNSUCCESSFUL;
            }

            if (!remote_address.is_ipv4())
            {
                throw std::runtime_error("Unsupported address family");
            }

            pending_connection pending{};
            pending.remote_address = remote_address;
            pending.accepted_socket = std::move(accepted_socket_ptr);

            LONG sequence = next_sequence_++;
            pending_connections_.try_emplace(sequence, std::move(pending));

            AFD_LISTEN_RESPONSE_INFO response{};
            response.Sequence = sequence;

            auto transport_buffer = convert_to_win_address(win_emu, remote_address);
            memcpy(&response.RemoteAddress, transport_buffer.data(), sizeof(win_sockaddr));

            win_emu.emu().write_memory<AFD_LISTEN_RESPONSE_INFO>(c.output_buffer, response);

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = sizeof(AFD_LISTEN_RESPONSE_INFO);
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_accept(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            if (c.input_buffer_length < sizeof(AFD_ACCEPT_INFO))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto accept_info = win_emu.emu().read_memory<AFD_ACCEPT_INFO>(c.input_buffer);

            const auto it = pending_connections_.find(accept_info.Sequence);
            if (it == pending_connections_.end())
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto& accepted_socket = it->second.accepted_socket;

            auto* target_device = win_emu.process.devices.get(accept_info.AcceptHandle);
            if (!target_device)
            {
                return STATUS_INVALID_HANDLE;
            }

            auto* target_endpoint = target_device->get_internal_device<afd_endpoint>();
            if (!target_endpoint)
            {
                return STATUS_INVALID_HANDLE;
            }

            target_endpoint->s_ = std::move(accepted_socket);

            pending_connections_.erase(it);

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_receive(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            auto& emu = win_emu.emu();

            if (c.input_buffer_length < sizeof(AFD_RECV_INFO<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto receive_info = emu.read_memory<AFD_RECV_INFO<EmulatorTraits<Emu64>>>(c.input_buffer);

            if (!receive_info.BufferArray || receive_info.BufferCount == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (receive_info.BufferCount > 1)
            {
                // TODO: Scatter/Gather
                return STATUS_NOT_SUPPORTED;
            }

            const auto wsabuf = emu.read_memory<EMU_WSABUF<EmulatorTraits<Emu64>>>(receive_info.BufferArray);
            if (!wsabuf.buf || wsabuf.len == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::vector<std::byte> host_buffer;
            host_buffer.resize(wsabuf.len);

            const auto bytes_received = this->s_->recv(host_buffer);

            if (bytes_received < 0)
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, true);
                    return STATUS_PENDING;
                }

                if (error == SERR(ECONNRESET))
                {
                    return STATUS_CONNECTION_RESET;
                }

                return STATUS_UNSUCCESSFUL;
            }

            emu.write_memory(wsabuf.buf, host_buffer.data(), static_cast<size_t>(bytes_received));

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = static_cast<uint32_t>(bytes_received);
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_send(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            auto& emu = win_emu.emu();

            if (c.input_buffer_length < sizeof(AFD_SEND_INFO<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto send_info = emu.read_memory<AFD_SEND_INFO<EmulatorTraits<Emu64>>>(c.input_buffer);

            if (!send_info.BufferArray || send_info.BufferCount == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (send_info.BufferCount > 1)
            {
                // TODO: Scatter/Gather
                return STATUS_NOT_SUPPORTED;
            }

            const auto wsabuf = emu.read_memory<EMU_WSABUF<EmulatorTraits<Emu64>>>(send_info.BufferArray);
            if (!wsabuf.buf || wsabuf.len == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::vector<std::byte> host_buffer;
            host_buffer.resize(wsabuf.len);

            emu.read_memory(wsabuf.buf, host_buffer.data(), host_buffer.size());

            const auto bytes_sent = this->s_->send(host_buffer);

            if (bytes_sent < 0)
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, false);
                    return STATUS_PENDING;
                }

                if (error == SERR(ECONNRESET))
                {
                    return STATUS_CONNECTION_RESET;
                }

                return STATUS_UNSUCCESSFUL;
            }

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = static_cast<uint32_t>(bytes_sent);
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        static std::vector<const afd_endpoint*> resolve_endpoints(windows_emulator& win_emu,
                                                                  const std::span<const AFD_POLL_HANDLE_INFO64> handles)
        {
            auto& proc = win_emu.process;

            std::vector<const afd_endpoint*> endpoints{};
            endpoints.reserve(handles.size());

            for (const auto& handle : handles)
            {
                auto* device = proc.devices.get(handle.Handle);
                if (!device)
                {
                    throw std::runtime_error("Bad device!");
                }

                const auto* endpoint = device->get_internal_device<afd_endpoint>();
                if (!endpoint || !endpoint->s_)
                {
                    throw std::runtime_error("Invalid AFD endpoint!");
                }

                endpoints.push_back(endpoint);
            }

            return endpoints;
        }

        static NTSTATUS perform_poll(windows_emulator& win_emu, const io_device_context& c,
                                     const std::span<const afd_endpoint* const> endpoints,
                                     const std::span<const AFD_POLL_HANDLE_INFO64> handles)
        {
            std::vector<network::poll_entry> poll_data{};
            poll_data.resize(endpoints.size());

            for (size_t i = 0; i < endpoints.size() && i < handles.size(); ++i)
            {
                auto& pfd = poll_data.at(i);
                const auto& handle = handles[i];

                pfd.s = endpoints[i]->s_.get();
                pfd.events = map_afd_request_events_to_socket(handle.PollEvents);
                pfd.revents = pfd.events;
            }

            const auto count = win_emu.socket_factory().poll_sockets(poll_data);
            if (count <= 0)
            {
                return STATUS_PENDING;
            }

            constexpr auto info_size = offsetof(AFD_POLL_INFO64, Handles);
            const emulator_object<AFD_POLL_HANDLE_INFO64> handle_info_obj{win_emu.emu(), c.input_buffer + info_size};

            size_t current_index = 0;

            for (size_t i = 0; i < endpoints.size(); ++i)
            {
                const auto& pfd = poll_data.at(i);
                if (pfd.revents == 0)
                {
                    continue;
                }

                const auto& handle = handles[i];
                const auto& endpoint = endpoints[i];

                const bool is_connecting =
                    endpoint->delayed_ioctl_ && _AFD_REQUEST(endpoint->delayed_ioctl_->io_control_code) == AFD_CONNECT;

                auto entry = handle_info_obj.read(i);
                entry.PollEvents = map_socket_response_events_to_afd(pfd.revents, handle.PollEvents, pfd.s->is_listening(), is_connecting);
                entry.Status = STATUS_SUCCESS;

                handle_info_obj.write(entry, current_index++);
            }

            assert(current_index == static_cast<size_t>(count));

            const emulator_object<AFD_POLL_INFO64> info_obj{win_emu.emu(), c.input_buffer};
            info_obj.access([&](AFD_POLL_INFO64& info) {
                info.NumberOfHandles = static_cast<ULONG>(current_index); //
            });

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = info_size + sizeof(AFD_POLL_HANDLE_INFO64) * current_index;
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_poll(windows_emulator& win_emu, const io_device_context& c)
        {
            const auto [info, handles] = get_poll_info(win_emu, c);
            const auto endpoints = resolve_endpoints(win_emu, handles);

            const auto status = perform_poll(win_emu, c, endpoints, handles);
            if (status != STATUS_PENDING)
            {
                return status;
            }

            if (!this->executing_delayed_ioctl_)
            {
                const auto timeout_callback = [](windows_emulator& win_emu, const io_device_context& c) {
                    const emulator_object<AFD_POLL_INFO64> info_obj{win_emu.emu(), c.input_buffer};
                    info_obj.access([&](AFD_POLL_INFO64& poll_info) {
                        poll_info.NumberOfHandles = 0; //
                    });
                };

                if (!info.Timeout.QuadPart)
                {
                    if (status == STATUS_PENDING)
                    {
                        timeout_callback(win_emu, c);
                        return STATUS_TIMEOUT;
                    }
                    return STATUS_SUCCESS;
                }

                std::optional<std::chrono::steady_clock::time_point> timeout{};
                if (info.Timeout.QuadPart != std::numeric_limits<int64_t>::max())
                {
                    timeout = utils::convert_delay_interval_to_time_point(win_emu.clock(), info.Timeout);
                }

                this->delay_ioctrl(c, {}, timeout, timeout_callback);
            }

            return STATUS_PENDING;
        }

        NTSTATUS ioctl_receive_datagram(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            auto& emu = win_emu.emu();

            if (c.input_buffer_length < sizeof(AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto receive_info = emu.read_memory<AFD_RECV_DATAGRAM_INFO<EmulatorTraits<Emu64>>>(c.input_buffer);
            const auto buffer = emu.read_memory<EMU_WSABUF<EmulatorTraits<Emu64>>>(receive_info.BufferArray);

            if (!buffer.len || buffer.len > 0x10000 || !buffer.buf)
            {
                return STATUS_INVALID_PARAMETER;
            }

            network::address from{};
            std::vector<std::byte> data{};
            data.resize(buffer.len);

            const auto recevied_data = this->s_->recvfrom(from, data);

            if (recevied_data < 0)
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, true);
                    return STATUS_PENDING;
                }

                return STATUS_UNSUCCESSFUL;
            }

            const auto data_size = std::min(data.size(), static_cast<size_t>(recevied_data));
            emu.write_memory(buffer.buf, data.data(), data_size);

            const auto win_from = convert_to_win_address(win_emu, from);

            if (receive_info.Address && receive_info.AddressLength)
            {
                const emulator_object<ULONG> address_length{emu, receive_info.AddressLength};
                const auto address_size = std::min(win_from.size(), static_cast<size_t>(address_length.read()));

                emu.write_memory(receive_info.Address, win_from.data(), address_size);
                address_length.write(static_cast<ULONG>(address_size));
            }

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = static_cast<uint32_t>(recevied_data);
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_send_datagram(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            const auto& emu = win_emu.emu();

            if (c.input_buffer_length < sizeof(AFD_SEND_DATAGRAM_INFO<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto send_info = emu.read_memory<AFD_SEND_DATAGRAM_INFO<EmulatorTraits<Emu64>>>(c.input_buffer);
            const auto buffer = emu.read_memory<EMU_WSABUF<EmulatorTraits<Emu64>>>(send_info.BufferArray);

            auto address_buffer =
                emu.read_memory(send_info.TdiConnInfo.RemoteAddress, static_cast<size_t>(send_info.TdiConnInfo.RemoteAddressLength));

            const auto target = convert_to_host_address(win_emu, address_buffer);
            const auto data = emu.read_memory(buffer.buf, buffer.len);

            const auto sent_data = this->s_->sendto(target, data);
            if (sent_data < 0)
            {
                const auto error = this->s_->get_last_error();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, false);
                    return STATUS_PENDING;
                }

                return STATUS_UNSUCCESSFUL;
            }

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = static_cast<uint32_t>(sent_data);
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_get_address(windows_emulator& win_emu, const io_device_context& c) const
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            const auto local_address = this->s_->get_local_address();
            if (!local_address)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::vector<std::byte> win_addr_bytes = convert_to_win_address(win_emu, *local_address);

            if (c.output_buffer_length < win_addr_bytes.size())
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            win_emu.emu().write_memory(c.output_buffer, win_addr_bytes.data(), win_addr_bytes.size());

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = static_cast<ULONG>(win_addr_bytes.size());
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_event_select(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            if (c.input_buffer_length < sizeof(AFD_EVENT_SELECT_INFO))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto select_info = win_emu.emu().read_memory<AFD_EVENT_SELECT_INFO>(c.input_buffer);

            this->event_select_event_ = select_info.Event;
            this->event_select_mask_ = select_info.PollEvents;
            this->triggered_events_ = 0;

            if (auto* event = win_emu.process.events.get(select_info.Event))
            {
                event->signaled = false;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS ioctl_enum_network_events(windows_emulator& win_emu, const io_device_context& c)
        {
            if (!this->s_)
            {
                throw std::runtime_error("Invalid AFD endpoint socket!");
            }

            if (c.output_buffer_length < 56)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            if (c.input_buffer)
            {
                if (c.input_buffer_length == 0)
                {
                    handle h{};
                    h.bits = c.input_buffer;

                    if (auto* event = win_emu.process.events.get(h))
                    {
                        event->signaled = false;
                    }
                }
                else
                {
                    return STATUS_NOT_SUPPORTED;
                }
            }

            win_emu.emu().write_memory(c.output_buffer, this->triggered_events_);
            this->triggered_events_ = 0;

            if (c.io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Information = 56;
                c.io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }
    };

    struct afd_async_connect_hlp : stateless_device
    {
        NTSTATUS io_control(windows_emulator& win_emu, const io_device_context& c) override
        {
            if (c.io_control_code != 0x12007)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (c.input_buffer_length < 40)
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const auto target_handle = win_emu.emu().read_memory<handle>(c.input_buffer + 16);

            auto* target_device = win_emu.process.devices.get(target_handle);
            if (!target_device)
            {
                return STATUS_INVALID_HANDLE;
            }

            auto* target_endpoint = target_device->get_internal_device<afd_endpoint>();
            if (!target_endpoint)
            {
                return STATUS_INVALID_HANDLE;
            }

            return target_endpoint->execute_ioctl(win_emu, c);
        }
    };
}

std::unique_ptr<io_device> create_afd_endpoint()
{
    return std::make_unique<afd_endpoint>();
}

std::unique_ptr<io_device> create_afd_async_connect_hlp()
{
    return std::make_unique<afd_async_connect_hlp>();
}
