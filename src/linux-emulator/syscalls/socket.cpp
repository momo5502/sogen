#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"
#include "../linux_socket.hpp"

#include <network/address.hpp>
#include <network/tcp_client_socket.hpp>
#include <network/udp_socket.hpp>

#include <cstring>

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    namespace
    {
        constexpr int SOCK_TYPE_MASK = 0xF;

        // Linux sockaddr_in (16 bytes)
#pragma pack(push, 1)
        struct linux_sockaddr_in
        {
            uint16_t sin_family;
            uint16_t sin_port;
            uint32_t sin_addr;
            uint8_t sin_zero[8];
        };
#pragma pack(pop)

        static_assert(sizeof(linux_sockaddr_in) == 16);

        bool read_sockaddr_in(const linux_syscall_context& c, const uint64_t addr_ptr, const uint32_t addrlen, linux_sockaddr_in& sin)
        {
            if (addr_ptr == 0 || addrlen < sizeof(uint16_t))
            {
                return false;
            }

            uint16_t family{};
            c.emu.read_memory(addr_ptr, &family, sizeof(family));
            if (family != AF_INET || addrlen < sizeof(linux_sockaddr_in))
            {
                return false;
            }

            c.emu.read_memory(addr_ptr, &sin, sizeof(sin));
            return true;
        }

        network::address make_host_address(const linux_emulator& emu_ref, const linux_sockaddr_in& sin)
        {
            sockaddr_in host_addr{};
            host_addr.sin_family = AF_INET;
            host_addr.sin_port = htons(emu_ref.get_host_port(ntohs(sin.sin_port)));
            std::memcpy(&host_addr.sin_addr, &sin.sin_addr, sizeof(host_addr.sin_addr));
            return network::address{host_addr};
        }

        void write_sockaddr_in(x86_64_emulator& emu, const uint64_t addr_ptr, const uint64_t addrlen_ptr, const uint16_t port,
                               const uint32_t addr)
        {
            linux_sockaddr_in sin{};
            sin.sin_family = AF_INET;
            sin.sin_port = port;
            sin.sin_addr = addr;

            if (addr_ptr != 0)
            {
                emu.write_memory(addr_ptr, &sin, sizeof(sin));
            }

            if (addrlen_ptr != 0)
            {
                uint32_t len = sizeof(linux_sockaddr_in);
                emu.write_memory(addrlen_ptr, &len, sizeof(len));
            }
        }

        int64_t map_host_socket_error()
        {
            switch (GET_SOCKET_ERROR())
            {
            case EWOULDBLOCK:
                return -LINUX_EAGAIN;
            case ECONNREFUSED:
                return -LINUX_ECONNREFUSED;
            case ECONNRESET:
                return -LINUX_ECONNRESET;
            case ETIMEDOUT:
                return -LINUX_ETIMEDOUT;
            case EINPROGRESS:
                return -LINUX_EINPROGRESS;
            case EADDRINUSE:
                return -LINUX_EADDRINUSE;
            case EADDRNOTAVAIL:
                return -LINUX_EADDRNOTAVAIL;
            case ENETUNREACH:
                return -LINUX_ENETUNREACH;
            case EHOSTUNREACH:
                return -LINUX_EHOSTUNREACH;
            case ENOTCONN:
                return -LINUX_ENOTCONN;
            default:
                return -LINUX_EINVAL;
            }
        }

        std::unique_ptr<network::socket> create_host_socket(const int domain, const int type)
        {
            if (domain == AF_INET || domain == AF_INET6)
            {
                if (type == SOCK_STREAM)
                {
                    return std::make_unique<network::tcp_client_socket>(domain);
                }

                if (type == SOCK_DGRAM)
                {
                    return std::make_unique<network::udp_socket>(domain);
                }
            }

            return nullptr;
        }
    }

    void sys_socket(const linux_syscall_context& c)
    {
        const auto domain = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto type_raw = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto protocol = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        const auto type = type_raw & SOCK_TYPE_MASK;
        const bool cloexec = (type_raw & SOCK_CLOEXEC) != 0;

        if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
        {
            write_linux_syscall_result(c, -LINUX_EAFNOSUPPORT);
            return;
        }

        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::socket;
        new_fd.close_on_exec = cloexec;
        new_fd.flags = type_raw & ~SOCK_TYPE_MASK;

        const auto fd_num = c.proc.fds.allocate(std::move(new_fd));

        linux_socket_state state{};
        state.domain = domain;
        state.type = type;
        state.protocol = protocol;
        c.emu_ref.sockets_.emplace(fd_num, std::move(state));

        write_linux_syscall_result(c, fd_num);
    }

    void sys_connect(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
        const auto addrlen = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (state->domain == AF_UNIX)
        {
            state->connected = true;
            write_linux_syscall_result(c, 0);
            return;
        }

        linux_sockaddr_in sin{};
        if (!read_sockaddr_in(c, addr_ptr, addrlen, sin))
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        auto host_socket = create_host_socket(state->domain, state->type);
        if (!host_socket)
        {
            write_linux_syscall_result(c, -LINUX_EPROTONOSUPPORT);
            return;
        }

        host_socket->set_blocking(false);
        const auto target = make_host_address(c.emu_ref, sin);

        bool connected = false;
        if (auto* tcp = dynamic_cast<network::tcp_client_socket*>(host_socket.get()))
        {
            connected = tcp->connect(target);
        }
        else
        {
            connected = true;
        }

        if (!connected)
        {
            write_linux_syscall_result(c, map_host_socket_error());
            return;
        }

        state->host_socket = std::move(host_socket);
        state->connected = true;
        state->peer_port = sin.sin_port;
        state->peer_addr = sin.sin_addr;
        state->so_error = 0;

        write_linux_syscall_result(c, 0);
    }

    void sys_accept(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        write_linux_syscall_result(c, -LINUX_EAGAIN);
    }

    void sys_accept4(const linux_syscall_context& c)
    {
        sys_accept(c);
    }

    void sys_sendto(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto len = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
        const auto dest_addr = get_linux_syscall_argument(c.emu, 4);
        const auto addrlen = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 5));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr || !state->host_socket)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        std::vector<std::byte> buffer(len);
        if (len > 0)
        {
            c.emu.read_memory(buf_addr, buffer.data(), len);
        }

        sent_size sent = -1;
        if (dest_addr != 0)
        {
            linux_sockaddr_in sin{};
            if (!read_sockaddr_in(c, dest_addr, addrlen, sin))
            {
                write_linux_syscall_result(c, -LINUX_EINVAL);
                return;
            }

            const auto target = make_host_address(c.emu_ref, sin);
            if (auto* udp = dynamic_cast<network::udp_socket*>(state->host_socket.get()))
            {
                if (!udp->send(target, buffer.data(), buffer.size()))
                {
                    write_linux_syscall_result(c, map_host_socket_error());
                    return;
                }

                sent = static_cast<sent_size>(len);
            }
            else
            {
                write_linux_syscall_result(c, -LINUX_EOPNOTSUPP);
                return;
            }
        }
        else
        {
            sent = ::send(state->host_socket->get_socket(), buffer.data(), static_cast<send_size>(buffer.size()), 0);
            if (sent < 0)
            {
                write_linux_syscall_result(c, map_host_socket_error());
                return;
            }
        }

        write_linux_syscall_result(c, sent);
    }

    void sys_recvfrom(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto len = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
        const auto src_addr = get_linux_syscall_argument(c.emu, 4);
        const auto addrlen_ptr = get_linux_syscall_argument(c.emu, 5);

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr || !state->host_socket)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        std::vector<std::byte> buffer(len);
        sent_size received = ::recv(state->host_socket->get_socket(), buffer.data(), static_cast<send_size>(buffer.size()), 0);
        if (received < 0)
        {
            write_linux_syscall_result(c, map_host_socket_error());
            return;
        }

        if (received == 0)
        {
            state->connected = false;
            write_linux_syscall_result(c, 0);
            return;
        }

        if (len > 0)
        {
            c.emu.write_memory(buf_addr, buffer.data(), static_cast<size_t>(received));
        }

        if (src_addr != 0 && state->domain == AF_INET)
        {
            write_sockaddr_in(c.emu, src_addr, addrlen_ptr, state->peer_port, state->peer_addr);
        }

        write_linux_syscall_result(c, received);
    }

    void sys_sendmsg(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, -LINUX_ENOTCONN);
    }

    void sys_recvmsg(const linux_syscall_context& c)
    {
        write_linux_syscall_result(c, -LINUX_EAGAIN);
    }

    void sys_shutdown(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state != nullptr)
        {
            if (state->host_socket)
            {
                state->host_socket->close();
                state->host_socket.reset();
            }

            state->connected = false;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_bind(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
        const auto addrlen = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 2));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        linux_sockaddr_in sin{};
        if (read_sockaddr_in(c, addr_ptr, addrlen, sin))
        {
            state->bound_port = sin.sin_port;
            state->bound_addr = sin.sin_addr;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_listen(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (auto* state = c.emu_ref.sockets_.get(sockfd))
        {
            state->listening = true;
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_setsockopt(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto level = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto optname = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        const auto optval_addr = get_linux_syscall_argument(c.emu, 3);
        const auto optlen = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 4));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        int val = 0;
        if (optval_addr != 0 && optlen >= sizeof(int))
        {
            c.emu.read_memory(optval_addr, &val, sizeof(val));
        }

        if (level == SOL_SOCKET)
        {
            switch (optname)
            {
            case SO_REUSEADDR:
                state->so_reuseaddr = val;
                break;
            case SO_REUSEPORT:
                state->so_reuseport = val;
                break;
            case SO_KEEPALIVE:
                state->so_keepalive = val;
                break;
            case SO_SNDBUF:
                state->so_sndbuf = val;
                break;
            case SO_RCVBUF:
                state->so_rcvbuf = val;
                break;
            default:
                break;
            }
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_getsockopt(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto level = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto optname = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        const auto optval_addr = get_linux_syscall_argument(c.emu, 3);
        const auto optlen_addr = get_linux_syscall_argument(c.emu, 4);

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        int val = 0;

        if (level == SOL_SOCKET)
        {
            switch (optname)
            {
            case SO_REUSEADDR:
                val = state->so_reuseaddr;
                break;
            case SO_REUSEPORT:
                val = state->so_reuseport;
                break;
            case SO_KEEPALIVE:
                val = state->so_keepalive;
                break;
            case SO_SNDBUF:
                val = state->so_sndbuf;
                break;
            case SO_RCVBUF:
                val = state->so_rcvbuf;
                break;
            case SO_TYPE:
                val = state->type;
                break;
            case SO_ERROR:
                val = state->so_error;
                state->so_error = 0;
                break;
            default:
                val = 0;
                break;
            }
        }

        if (optval_addr != 0)
        {
            c.emu.write_memory(optval_addr, &val, sizeof(val));
        }
        if (optlen_addr != 0)
        {
            uint32_t optlen = sizeof(int);
            c.emu.write_memory(optlen_addr, &optlen, sizeof(optlen));
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_socketpair(const linux_syscall_context& c)
    {
        const auto domain = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto type_raw = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto protocol = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        const auto sv_addr = get_linux_syscall_argument(c.emu, 3);

        const auto type = type_raw & SOCK_TYPE_MASK;
        const bool cloexec = (type_raw & SOCK_CLOEXEC) != 0;

        if (domain != AF_UNIX)
        {
            write_linux_syscall_result(c, -LINUX_EAFNOSUPPORT);
            return;
        }

        linux_fd fd1{};
        fd1.type = fd_type::socket;
        fd1.close_on_exec = cloexec;

        linux_fd fd2{};
        fd2.type = fd_type::socket;
        fd2.close_on_exec = cloexec;

        const auto fd1_num = c.proc.fds.allocate(std::move(fd1));
        const auto fd2_num = c.proc.fds.allocate(std::move(fd2));

        linux_socket_state ss1{};
        ss1.domain = domain;
        ss1.type = type;
        ss1.protocol = protocol;
        ss1.connected = true;
        c.emu_ref.sockets_.emplace(fd1_num, std::move(ss1));

        linux_socket_state ss2{};
        ss2.domain = domain;
        ss2.type = type;
        ss2.protocol = protocol;
        ss2.connected = true;
        c.emu_ref.sockets_.emplace(fd2_num, std::move(ss2));

        int32_t sv[2] = {fd1_num, fd2_num};
        c.emu.write_memory(sv_addr, sv, sizeof(sv));

        write_linux_syscall_result(c, 0);
    }

    void sys_getsockname(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
        const auto addrlen_ptr = get_linux_syscall_argument(c.emu, 2);

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (state->domain == AF_INET)
        {
            write_sockaddr_in(c.emu, addr_ptr, addrlen_ptr, state->bound_port, state->bound_addr);
        }
        else
        {
            auto family = static_cast<uint16_t>(state->domain);
            c.emu.write_memory(addr_ptr, &family, sizeof(family));

            uint32_t len = sizeof(uint16_t);
            c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
        }

        write_linux_syscall_result(c, 0);
    }

    void sys_getpeername(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
        const auto addrlen_ptr = get_linux_syscall_argument(c.emu, 2);

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = c.emu_ref.sockets_.get(sockfd);
        if (state == nullptr || !state->connected)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        if (state->domain == AF_INET)
        {
            write_sockaddr_in(c.emu, addr_ptr, addrlen_ptr, state->peer_port, state->peer_addr);
        }
        else
        {
            auto family = static_cast<uint16_t>(state->domain);
            c.emu.write_memory(addr_ptr, &family, sizeof(family));

            uint32_t len = sizeof(uint16_t);
            c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
        }

        write_linux_syscall_result(c, 0);
    }

} // namespace sogen
