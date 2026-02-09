#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <cstring>

using namespace linux_errno;

// Linux socket constants
namespace
{
    // Address families
    constexpr int AF_UNIX = 1;
    constexpr int AF_INET = 2;
    constexpr int AF_INET6 = 10;

    // Socket types (lower bits)
    constexpr int SOCK_STREAM = 1;
    constexpr int SOCK_DGRAM = 2;
    constexpr int SOCK_RAW = 3;
    constexpr int SOCK_TYPE_MASK = 0xF;

    // Socket type flags (upper bits)
    constexpr int SOCK_NONBLOCK = 04000;
    constexpr int SOCK_CLOEXEC = 02000000;

    // Shutdown how
    // constexpr int SHUT_RD = 0;
    // constexpr int SHUT_WR = 1;
    // constexpr int SHUT_RDWR = 2;

    // SOL_SOCKET
    constexpr int SOL_SOCKET = 1;

    // Socket options
    constexpr int SO_REUSEADDR = 2;
    constexpr int SO_TYPE = 3;
    constexpr int SO_ERROR = 4;
    constexpr int SO_KEEPALIVE = 9;
    constexpr int SO_REUSEPORT = 15;
    constexpr int SO_SNDBUF = 7;
    constexpr int SO_RCVBUF = 8;

    // Message flags
    // constexpr int MSG_DONTWAIT = 0x40;

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

    struct socket_state
    {
        int domain{};
        int type{};
        int protocol{};
        bool listening{};
        bool connected{};
        int so_reuseaddr{};
        int so_keepalive{};
        int so_reuseport{};
        int so_sndbuf{65536};
        int so_rcvbuf{65536};
        int so_error{};

        // Bound address info
        uint16_t bound_port{};
        uint32_t bound_addr{};
    };

    // Simple socket state storage — maps fd to socket metadata
    // We store this alongside the fd_table since linux_fd doesn't have socket state
    std::map<int, socket_state> g_socket_states;
}

void sys_socket(const linux_syscall_context& c)
{
    const auto domain = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto type_raw = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto protocol = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    const auto type = type_raw & SOCK_TYPE_MASK;
    const bool cloexec = (type_raw & SOCK_CLOEXEC) != 0;

    // Validate domain
    if (domain != AF_UNIX && domain != AF_INET && domain != AF_INET6)
    {
        write_linux_syscall_result(c, -LINUX_EAFNOSUPPORT);
        return;
    }

    // Validate type
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

    // Track socket state
    socket_state ss{};
    ss.domain = domain;
    ss.type = type;
    ss.protocol = protocol;
    g_socket_states[fd_num] = ss;

    write_linux_syscall_result(c, fd_num);
}

void sys_connect(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    // addr is arg 1, addrlen is arg 2

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // We don't actually connect anywhere — stub that returns ECONNREFUSED
    // In a full implementation, we'd proxy to the host's network stack
    write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
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

    // No actual network — return EAGAIN (non-blocking) or block forever
    // Return EAGAIN so we don't deadlock
    write_linux_syscall_result(c, -LINUX_EAGAIN);
}

void sys_accept4(const linux_syscall_context& c)
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

void sys_sendto(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    // buf is arg 1, len is arg 2, flags is arg 3, dest_addr is arg 4, addrlen is arg 5
    const auto len = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end() || !it->second.connected)
    {
        write_linux_syscall_result(c, -LINUX_ENOTCONN);
        return;
    }

    // Pretend we sent everything
    write_linux_syscall_result(c, static_cast<int64_t>(len));
}

void sys_recvfrom(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // No data available
    write_linux_syscall_result(c, -LINUX_EAGAIN);
}

void sys_sendmsg(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    write_linux_syscall_result(c, -LINUX_ENOTCONN);
}

void sys_recvmsg(const linux_syscall_context& c)
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

void sys_shutdown(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // Clean up socket state
    auto it = g_socket_states.find(sockfd);
    if (it != g_socket_states.end())
    {
        it->second.connected = false;
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

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end())
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // Read the address family
    uint16_t family{};
    c.emu.read_memory(addr_ptr, &family, sizeof(family));

    if (family == AF_INET && addrlen >= sizeof(linux_sockaddr_in))
    {
        linux_sockaddr_in sin{};
        c.emu.read_memory(addr_ptr, &sin, sizeof(sin));
        it->second.bound_port = sin.sin_port;
        it->second.bound_addr = sin.sin_addr;
    }

    // Pretend bind succeeded
    write_linux_syscall_result(c, 0);
}

void sys_listen(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    // backlog is arg 1

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    auto it = g_socket_states.find(sockfd);
    if (it != g_socket_states.end())
    {
        it->second.listening = true;
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

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end())
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    int val = 0;
    if (optval_addr && optlen >= sizeof(int))
    {
        c.emu.read_memory(optval_addr, &val, sizeof(val));
    }

    if (level == SOL_SOCKET)
    {
        switch (optname)
        {
        case SO_REUSEADDR:
            it->second.so_reuseaddr = val;
            break;
        case SO_REUSEPORT:
            it->second.so_reuseport = val;
            break;
        case SO_KEEPALIVE:
            it->second.so_keepalive = val;
            break;
        case SO_SNDBUF:
            it->second.so_sndbuf = val;
            break;
        case SO_RCVBUF:
            it->second.so_rcvbuf = val;
            break;
        default:
            break;
        }
    }

    // Pretend all setsockopt calls succeed
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

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end())
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
            val = it->second.so_reuseaddr;
            break;
        case SO_REUSEPORT:
            val = it->second.so_reuseport;
            break;
        case SO_KEEPALIVE:
            val = it->second.so_keepalive;
            break;
        case SO_SNDBUF:
            val = it->second.so_sndbuf;
            break;
        case SO_RCVBUF:
            val = it->second.so_rcvbuf;
            break;
        case SO_TYPE:
            val = it->second.type;
            break;
        case SO_ERROR:
            val = it->second.so_error;
            it->second.so_error = 0; // Clear after reading
            break;
        default:
            val = 0;
            break;
        }
    }

    if (optval_addr)
    {
        c.emu.write_memory(optval_addr, &val, sizeof(val));
    }
    if (optlen_addr)
    {
        uint32_t len = sizeof(int);
        c.emu.write_memory(optlen_addr, &len, sizeof(len));
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

    // Create two connected socket fds
    linux_fd fd1{};
    fd1.type = fd_type::socket;
    fd1.close_on_exec = cloexec;

    linux_fd fd2{};
    fd2.type = fd_type::socket;
    fd2.close_on_exec = cloexec;

    const auto fd1_num = c.proc.fds.allocate(std::move(fd1));
    const auto fd2_num = c.proc.fds.allocate(std::move(fd2));

    socket_state ss1{};
    ss1.domain = domain;
    ss1.type = type;
    ss1.protocol = protocol;
    ss1.connected = true;
    g_socket_states[fd1_num] = ss1;

    socket_state ss2{};
    ss2.domain = domain;
    ss2.type = type;
    ss2.protocol = protocol;
    ss2.connected = true;
    g_socket_states[fd2_num] = ss2;

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

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end())
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    if (it->second.domain == AF_INET)
    {
        linux_sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = it->second.bound_port;
        sin.sin_addr = it->second.bound_addr;

        c.emu.write_memory(addr_ptr, &sin, sizeof(sin));

        uint32_t len = sizeof(linux_sockaddr_in);
        c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
    }
    else
    {
        // Return a minimal sockaddr with just the family
        uint16_t family = static_cast<uint16_t>(it->second.domain);
        c.emu.write_memory(addr_ptr, &family, sizeof(family));

        uint32_t len = sizeof(uint16_t);
        c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
    }

    write_linux_syscall_result(c, 0);
}

void sys_getpeername(const linux_syscall_context& c)
{
    const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));

    auto* fd_entry = c.proc.fds.get(sockfd);
    if (!fd_entry || fd_entry->type != fd_type::socket)
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    auto it = g_socket_states.find(sockfd);
    if (it == g_socket_states.end() || !it->second.connected)
    {
        write_linux_syscall_result(c, -LINUX_ENOTCONN);
        return;
    }

    // Return a synthetic peer address
    const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
    const auto addrlen_ptr = get_linux_syscall_argument(c.emu, 2);

    if (it->second.domain == AF_INET)
    {
        linux_sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = 0;
        sin.sin_addr = 0x7F000001; // 127.0.0.1

        c.emu.write_memory(addr_ptr, &sin, sizeof(sin));

        uint32_t len = sizeof(linux_sockaddr_in);
        c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
    }
    else
    {
        uint16_t family = static_cast<uint16_t>(it->second.domain);
        c.emu.write_memory(addr_ptr, &family, sizeof(family));

        uint32_t len = sizeof(uint16_t);
        c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
    }

    write_linux_syscall_result(c, 0);
}
