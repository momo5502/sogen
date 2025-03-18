#include "afd_endpoint.hpp"
#include "afd_types.hpp"

#include "../windows_emulator.hpp"

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

    std::pair<AFD_POLL_INFO64, std::vector<AFD_POLL_HANDLE_INFO64>> get_poll_info(windows_emulator& win_emu,
                                                                                  const io_device_context& c)
    {
        constexpr auto info_size = offsetof(AFD_POLL_INFO64, Handles);
        if (!c.input_buffer || c.input_buffer_length < info_size)
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

        if (poll_events & (AFD_POLL_ACCEPT | AFD_POLL_RECEIVE))
        {
            socket_events |= POLLRDNORM;
        }

        if (poll_events & AFD_POLL_RECEIVE_EXPEDITED)
        {
            socket_events |= POLLRDNORM;
        }

        if (poll_events & AFD_POLL_RECEIVE_EXPEDITED)
        {
            socket_events |= POLLRDBAND;
        }

        if (poll_events & (AFD_POLL_CONNECT_FAIL | AFD_POLL_SEND))
        {
            socket_events |= POLLWRNORM;
        }

        return socket_events;
    }

    ULONG map_socket_response_events_to_afd(const int16_t socket_events)
    {
        ULONG afd_events = 0;

        if (socket_events & POLLRDNORM)
        {
            afd_events |= (AFD_POLL_ACCEPT | AFD_POLL_RECEIVE);
        }

        if (socket_events & POLLRDBAND)
        {
            afd_events |= AFD_POLL_RECEIVE_EXPEDITED;
        }

        if (socket_events & POLLWRNORM)
        {
            afd_events |= (AFD_POLL_CONNECT_FAIL | AFD_POLL_SEND);
        }

        if ((socket_events & (POLLHUP | POLLERR)) == (POLLHUP | POLLERR))
        {
            afd_events |= (AFD_POLL_CONNECT_FAIL | AFD_POLL_ABORT);
        }
        else if (socket_events & POLLHUP)
        {
            afd_events |= AFD_POLL_DISCONNECT;
        }

        if (socket_events & POLLNVAL)
        {
            afd_events |= AFD_POLL_LOCAL_CLOSE;
        }

        return afd_events;
    }

    NTSTATUS perform_poll(windows_emulator& win_emu, const io_device_context& c,
                          const std::span<const SOCKET> endpoints,
                          const std::span<const AFD_POLL_HANDLE_INFO64> handles)
    {
        std::vector<pollfd> poll_data{};
        poll_data.resize(endpoints.size());

        for (size_t i = 0; i < endpoints.size() && i < handles.size(); ++i)
        {
            auto& pfd = poll_data.at(i);
            const auto& handle = handles[i];

            pfd.fd = endpoints[i];
            pfd.events = map_afd_request_events_to_socket(handle.PollEvents);
            pfd.revents = pfd.events;
        }

        const auto count = poll(poll_data.data(), static_cast<uint32_t>(poll_data.size()), 0);
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

            auto entry = handle_info_obj.read(i);
            entry.PollEvents = map_socket_response_events_to_afd(pfd.revents);
            entry.Status = STATUS_SUCCESS;

            handle_info_obj.write(entry, current_index++);
            break;
        }

        assert(current_index == static_cast<size_t>(count));

        emulator_object<AFD_POLL_INFO64>{win_emu.emu(), c.input_buffer}.access(
            [&](AFD_POLL_INFO64& info) { info.NumberOfHandles = static_cast<ULONG>(current_index); });

        return STATUS_SUCCESS;
    }

    struct afd_endpoint : io_device
    {
        bool executing_delayed_ioctl_{};
        std::optional<afd_creation_data> creation_data{};
        std::optional<SOCKET> s_{};
        std::optional<bool> require_poll_{};
        std::optional<io_device_context> delayed_ioctl_{};
        std::optional<std::chrono::steady_clock::time_point> timeout_{};

        afd_endpoint()
        {
            network::initialize_wsa();
        }

        afd_endpoint(afd_endpoint&&) = delete;
        afd_endpoint& operator=(afd_endpoint&&) = delete;

        ~afd_endpoint() override
        {
            if (this->s_)
            {
                closesocket(*this->s_);
            }
        }

        void create(windows_emulator& win_emu, const io_device_creation_data& data) override
        {
            this->creation_data = get_creation_data(win_emu, data);
            this->setup();
        }

        void setup()
        {
            if (!this->creation_data)
            {
                return;
            }

            const auto& data = *this->creation_data;

            const auto af = translate_win_to_host_address_family(data.address_family);
            const auto type = translate_win_to_host_type(data.type);
            const auto protocol = translate_win_to_host_protocol(data.protocol);

            const auto sock = socket(af, type, protocol);
            if (sock == INVALID_SOCKET)
            {
                throw std::runtime_error("Failed to create socket!");
            }

            network::socket::set_blocking(sock, false);

            this->s_ = sock;
        }

        void delay_ioctrl(const io_device_context& c,
                          const std::optional<std::chrono::steady_clock::time_point> timeout = {},
                          const std::optional<bool> require_poll = {})
        {
            if (this->executing_delayed_ioctl_)
            {
                return;
            }

            this->timeout_ = timeout;
            this->require_poll_ = require_poll;
            this->delayed_ioctl_ = c;
        }

        void clear_pending_state()
        {
            this->timeout_ = {};
            this->require_poll_ = {};
            this->delayed_ioctl_ = {};
        }

        void work(windows_emulator& win_emu) override
        {
            if (!this->delayed_ioctl_ || !this->s_)
            {
                return;
            }

            this->executing_delayed_ioctl_ = true;
            const auto _ = utils::finally([&] { this->executing_delayed_ioctl_ = false; });

            if (this->require_poll_.has_value())
            {
                const auto is_ready = network::socket::is_socket_ready(*this->s_, *this->require_poll_);
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
            }

            auto* e = win_emu.process.events.get(this->delayed_ioctl_->event);
            if (e)
            {
                e->signaled = true;
            }

            this->clear_pending_state();
        }

        void deserialize_object(utils::buffer_deserializer& buffer) override
        {
            buffer.read_optional(this->creation_data);
            this->setup();

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
                win_emu.log.print(color::cyan, "Bad AFD IOCTL: %X\n", c.io_control_code);
                return STATUS_NOT_SUPPORTED;
            }

            win_emu.log.print(color::cyan, "AFD IOCTL: %X\n", c.io_control_code);

            const auto request = _AFD_REQUEST(c.io_control_code);

            switch (request)
            {
            case AFD_BIND:
                return this->ioctl_bind(win_emu, c);
            case AFD_SEND_DATAGRAM:
                return this->ioctl_send_datagram(win_emu, c);
            case AFD_RECEIVE_DATAGRAM:
                return this->ioctl_receive_datagram(win_emu, c);
            case AFD_POLL:
                return this->ioctl_poll(win_emu, c);
            case AFD_SET_CONTEXT:
            case AFD_GET_INFORMATION:
                return STATUS_SUCCESS;
            default:
                win_emu.log.print(color::gray, "Unsupported AFD IOCTL: %X\n", c.io_control_code);
                return STATUS_NOT_SUPPORTED;
            }
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

            if (bind(*this->s_, &addr.get_addr(), addr.get_size()) == SOCKET_ERROR)
            {
                return STATUS_ADDRESS_ALREADY_ASSOCIATED;
            }

            return STATUS_SUCCESS;
        }

        static std::vector<SOCKET> resolve_endpoints(windows_emulator& win_emu,
                                                     const std::span<const AFD_POLL_HANDLE_INFO64> handles)
        {
            auto& proc = win_emu.process;

            std::vector<SOCKET> endpoints{};
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

                endpoints.push_back(*endpoint->s_);
            }

            return endpoints;
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
                if (!info.Timeout.QuadPart)
                {
                    return status;
                }

                std::optional<std::chrono::steady_clock::time_point> timeout{};
                if (info.Timeout.QuadPart != std::numeric_limits<int64_t>::max())
                {
                    timeout = utils::convert_delay_interval_to_time_point(win_emu.clock(), info.Timeout);
                }

                this->delay_ioctrl(c, timeout);
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
            auto from_length = from.get_max_size();

            std::vector<char> data{};
            data.resize(buffer.len);

            const auto recevied_data = recvfrom(*this->s_, data.data(), static_cast<send_size>(data.size()), 0,
                                                &from.get_addr(), &from_length);

            if (recevied_data < 0)
            {
                const auto error = GET_SOCKET_ERROR();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, {}, true);
                    return STATUS_PENDING;
                }

                return STATUS_UNSUCCESSFUL;
            }

            assert(from.get_size() == from_length);

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

            auto address_buffer = emu.read_memory(send_info.TdiConnInfo.RemoteAddress,
                                                  static_cast<size_t>(send_info.TdiConnInfo.RemoteAddressLength));

            const auto target = convert_to_host_address(win_emu, address_buffer);
            const auto data = emu.read_memory(buffer.buf, buffer.len);

            const auto sent_data =
                sendto(*this->s_, reinterpret_cast<const char*>(data.data()), static_cast<send_size>(data.size()),
                       0 /* TODO */, &target.get_addr(), target.get_size());

            if (sent_data < 0)
            {
                const auto error = GET_SOCKET_ERROR();
                if (error == SERR(EWOULDBLOCK))
                {
                    this->delay_ioctrl(c, {}, false);
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
    };
}

std::unique_ptr<io_device> create_afd_endpoint()
{
    return std::make_unique<afd_endpoint>();
}
