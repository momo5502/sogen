#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <cerrno>
#include <cstring>
#include <array>
#include <bit>
#if !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    // Linux socket constants
    namespace
    {
        // Address families
        constexpr int LINUX_AF_UNIX = 1;
        constexpr int LINUX_AF_INET = 2;
        constexpr int LINUX_AF_INET6 = 10;

        // Socket types (lower bits)
        constexpr int LINUX_SOCK_STREAM = 1;
        constexpr int LINUX_SOCK_DGRAM = 2;
        constexpr int LINUX_SOCK_RAW = 3;
        constexpr int LINUX_SOCK_TYPE_MASK = 0xF;

        // Socket type flags (upper bits)
        constexpr int LINUX_SOCK_CLOEXEC = 02000000;
#if !defined(_WIN32)
        constexpr int LINUX_SOCK_NONBLOCK = 04000;
#endif

        // Shutdown how
        // constexpr int SHUT_RD = 0;
        // constexpr int SHUT_WR = 1;
        // constexpr int SHUT_RDWR = 2;

        // LINUX_SOL_SOCKET
        constexpr int LINUX_SOL_SOCKET = 1;

        // Socket options
        constexpr int LINUX_SO_REUSEADDR = 2;
        constexpr int LINUX_SO_TYPE = 3;
        constexpr int LINUX_SO_ERROR = 4;
        constexpr int LINUX_SO_KEEPALIVE = 9;
        constexpr int LINUX_SO_REUSEPORT = 15;
        constexpr int LINUX_SO_SNDBUF = 7;
        constexpr int LINUX_SO_RCVBUF = 8;

        // Message flags
        // constexpr int MSG_DONTWAIT = 0x40;

        // Linux sockaddr_in (16 bytes)
        // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
#pragma pack(push, 1)
        struct linux_sockaddr_in
        {
            uint16_t sin_family;
            uint16_t sin_port;
            uint32_t sin_addr;
            uint8_t sin_zero[8];
        };
#pragma pack(pop)
        // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

        static_assert(sizeof(linux_sockaddr_in) == 16);

        uint16_t read_network_u16(const uint16_t value)
        {
            const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
            return static_cast<uint16_t>((static_cast<uint16_t>(bytes.at(0)) << 8U) | static_cast<uint16_t>(bytes.at(1)));
        }

        uint32_t read_network_u32(const uint32_t value)
        {
            const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(value)>>(value);
            return (static_cast<uint32_t>(bytes.at(0)) << 24U) | (static_cast<uint32_t>(bytes.at(1)) << 16U) |
                   (static_cast<uint32_t>(bytes.at(2)) << 8U) | static_cast<uint32_t>(bytes.at(3));
        }

#if !defined(_WIN32)
        uint16_t write_network_u16(const uint16_t value)
        {
            const std::array<uint8_t, sizeof(value)> bytes{
                static_cast<uint8_t>((value >> 8U) & 0xFFU),
                static_cast<uint8_t>(value & 0xFFU),
            };
            return std::bit_cast<uint16_t>(bytes);
        }
#endif

        uint32_t write_network_u32(const uint32_t value)
        {
            const std::array<uint8_t, sizeof(value)> bytes{
                static_cast<uint8_t>((value >> 24U) & 0xFFU),
                static_cast<uint8_t>((value >> 16U) & 0xFFU),
                static_cast<uint8_t>((value >> 8U) & 0xFFU),
                static_cast<uint8_t>(value & 0xFFU),
            };
            return std::bit_cast<uint32_t>(bytes);
        }
        linux_socket_state* socket_state_for_fd(linux_fd* fd_entry)
        {
            if (!fd_entry || fd_entry->type != fd_type::socket || !fd_entry->socket_state)
            {
                return nullptr;
            }

            return fd_entry->socket_state.get();
        }

        const linux_socket_state* socket_state_for_fd(const linux_fd* fd_entry)
        {
            if (!fd_entry || fd_entry->type != fd_type::socket || !fd_entry->socket_state)
            {
                return nullptr;
            }

            return fd_entry->socket_state.get();
        }
    }

    int64_t map_socket_errno_to_linux(const int host_errno)
    {
        switch (host_errno)
        {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
            return LINUX_EAGAIN;
        case EINTR:
            return LINUX_EINTR;
        case EBADF:
            return LINUX_EBADF;
        case EINVAL:
            return LINUX_EINVAL;
        case EPIPE:
            return LINUX_EPIPE;
#if defined(ECONNRESET)
        case ECONNRESET:
            return LINUX_ECONNRESET;
#endif
#if defined(ECONNREFUSED)
        case ECONNREFUSED:
            return LINUX_ECONNREFUSED;
#endif
#if defined(ENETUNREACH)
        case ENETUNREACH:
            return LINUX_ENETUNREACH;
#endif
#if defined(EHOSTUNREACH)
        case EHOSTUNREACH:
            return LINUX_EHOSTUNREACH;
#endif
#if defined(ETIMEDOUT)
        case ETIMEDOUT:
            return LINUX_ETIMEDOUT;
#endif
#if defined(ENOTCONN)
        case ENOTCONN:
            return LINUX_ENOTCONN;
#endif
        default:
            return LINUX_EIO;
        }
    }

    int host_send_flags(const int guest_flags)
    {
#if defined(MSG_NOSIGNAL)
        return guest_flags | MSG_NOSIGNAL;
#else
        return guest_flags;
#endif
    }

    void apply_host_socket_flags(linux_socket_state& state, const linux_fd& fd_entry)
    {
#if !defined(_WIN32)
        if (state.host_socket < 0)
        {
            return;
        }

        const auto current = ::fcntl(state.host_socket, F_GETFL, 0);
        if (current < 0)
        {
            return;
        }

        const auto desired = ((fd_entry.flags & LINUX_SOCK_NONBLOCK) != 0) ? (current | O_NONBLOCK) : (current & ~O_NONBLOCK);
        if (desired != current)
        {
            (void)::fcntl(state.host_socket, F_SETFL, desired);
        }
#else
        (void)state;
        (void)fd_entry;
#endif
    }

    bool linux_socket_has_host_socket(const linux_fd& fd_entry)
    {
        const auto* state = socket_state_for_fd(&fd_entry);
        return state != nullptr && state->host_socket >= 0;
    }

    void linux_socket_apply_fd_flags(const int fd, const linux_fd& fd_entry)
    {
        (void)fd;
        if (fd_entry.type == fd_type::socket && fd_entry.socket_state)
        {
            apply_host_socket_flags(*fd_entry.socket_state, fd_entry);
        }
    }

    int64_t linux_socket_send_guest_buffer(const linux_syscall_context& c, const int fd, const uint64_t buf_addr, const size_t count,
                                           const int flags)
    {
        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            return -LINUX_EBADF;
        }

        auto* state = socket_state_for_fd(fd_entry);
        if (!state || state->host_socket < 0)
        {
            return -LINUX_ENOTCONN;
        }
        apply_host_socket_flags(*state, *fd_entry);

        std::vector<uint8_t> buffer(count);
        if (count != 0)
        {
            c.emu.read_memory(buf_addr, buffer.data(), count);
        }

#if defined(_WIN32)
        (void)flags;
        return -LINUX_ECONNREFUSED;
#else
        errno = 0;
        const auto written = ::send(state->host_socket, buffer.data(), count, host_send_flags(flags));
        if (written < 0)
        {
            return -map_socket_errno_to_linux(errno);
        }

        return static_cast<int64_t>(written);
#endif
    }

    int64_t linux_socket_recv_guest_buffer(const linux_syscall_context& c, const int fd, const uint64_t buf_addr, const size_t count,
                                           const int flags)
    {
        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            return -LINUX_EBADF;
        }

        auto* state = socket_state_for_fd(fd_entry);
        if (!state || state->host_socket < 0)
        {
            return -LINUX_ENOTCONN;
        }
        apply_host_socket_flags(*state, *fd_entry);

        std::vector<uint8_t> buffer(count);
#if defined(_WIN32)
        (void)buf_addr;
        (void)flags;
        return -LINUX_ECONNREFUSED;
#else
        errno = 0;
        const auto bytes_read = ::recv(state->host_socket, buffer.data(), count, flags);
        if (bytes_read < 0)
        {
            return -map_socket_errno_to_linux(errno);
        }

        if (bytes_read > 0)
        {
            c.emu.write_memory(buf_addr, buffer.data(), static_cast<size_t>(bytes_read));
        }

        return static_cast<int64_t>(bytes_read);
#endif
    }

    int64_t linux_socket_writev_from_guest_with_flags(const linux_syscall_context& c, const int fd, const uint64_t iov_addr,
                                                      const int iovcnt, const int flags)
    {
        int64_t total_written = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            const auto entry_addr = iov_addr + static_cast<uint64_t>(i) * 16;
            uint64_t iov_base{};
            uint64_t iov_len{};
            c.emu.read_memory(entry_addr, &iov_base, 8);
            c.emu.read_memory(entry_addr + 8, &iov_len, 8);
            if (iov_len == 0)
            {
                continue;
            }

            const auto written = linux_socket_send_guest_buffer(c, fd, iov_base, static_cast<size_t>(iov_len), flags);
            if (written < 0)
            {
                return total_written > 0 ? total_written : written;
            }

            total_written += written;
            if (written < static_cast<int64_t>(iov_len))
            {
                break;
            }
        }

        return total_written;
    }

    int64_t linux_socket_writev_from_guest(const linux_syscall_context& c, const int fd, const uint64_t iov_addr, const int iovcnt)
    {
        return linux_socket_writev_from_guest_with_flags(c, fd, iov_addr, iovcnt, 0);
    }

    int64_t linux_socket_readv_to_guest_with_flags(const linux_syscall_context& c, const int fd, const uint64_t iov_addr, const int iovcnt,
                                                   const int flags)
    {
        int64_t total_read = 0;
        for (int i = 0; i < iovcnt; ++i)
        {
            const auto entry_addr = iov_addr + static_cast<uint64_t>(i) * 16;
            uint64_t iov_base{};
            uint64_t iov_len{};
            c.emu.read_memory(entry_addr, &iov_base, 8);
            c.emu.read_memory(entry_addr + 8, &iov_len, 8);
            if (iov_len == 0)
            {
                continue;
            }

            const auto bytes_read = linux_socket_recv_guest_buffer(c, fd, iov_base, static_cast<size_t>(iov_len), flags);
            if (bytes_read < 0)
            {
                return total_read > 0 ? total_read : bytes_read;
            }

            total_read += bytes_read;
            if (bytes_read < static_cast<int64_t>(iov_len))
            {
                break;
            }
        }

        return total_read;
    }

    int64_t linux_socket_readv_to_guest(const linux_syscall_context& c, const int fd, const uint64_t iov_addr, const int iovcnt)
    {
        return linux_socket_readv_to_guest_with_flags(c, fd, iov_addr, iovcnt, 0);
    }

    void sys_socket(const linux_syscall_context& c)
    {
        const auto domain = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto type_raw = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto protocol = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        const auto type = type_raw & LINUX_SOCK_TYPE_MASK;
        const bool cloexec = (type_raw & LINUX_SOCK_CLOEXEC) != 0;

        // Validate domain
        if (domain != LINUX_AF_UNIX && domain != LINUX_AF_INET && domain != LINUX_AF_INET6)
        {
            write_linux_syscall_result(c, -LINUX_EAFNOSUPPORT);
            return;
        }

        // Validate type
        if (type != LINUX_SOCK_STREAM && type != LINUX_SOCK_DGRAM && type != LINUX_SOCK_RAW)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        linux_fd new_fd{};
        new_fd.type = fd_type::socket;
        new_fd.close_on_exec = cloexec;
        new_fd.flags = type_raw & ~LINUX_SOCK_TYPE_MASK;
        new_fd.socket_state = std::make_shared<linux_socket_state>();
        new_fd.socket_state->domain = domain;
        new_fd.socket_state->type = type;
        new_fd.socket_state->protocol = protocol;

        const auto fd_num = c.proc.fds.allocate(std::move(new_fd));

        write_linux_syscall_result(c, fd_num);
    }

    void cleanup_linux_socket_state(const int fd)
    {
        (void)fd;
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

        auto* state = socket_state_for_fd(fd_entry);
        if (!state)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (state->domain != LINUX_AF_INET || state->type != LINUX_SOCK_STREAM || addrlen < sizeof(linux_sockaddr_in))
        {
            write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
            return;
        }

        linux_sockaddr_in sin{};
        c.emu.read_memory(addr_ptr, &sin, sizeof(sin));
        const auto guest_port = read_network_u16(sin.sin_port);
        const auto guest_addr = read_network_u32(sin.sin_addr);
        const auto host_port = c.emu_ref.port_mapper.get_host_port(guest_port);
        if (sin.sin_family != LINUX_AF_INET)
        {
            write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
            return;
        }

        if (guest_addr != 0x7F000001U)
        {
            write_linux_syscall_result(c, -LINUX_ENETUNREACH);
            return;
        }

        if (host_port == 0)
        {
            write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
            return;
        }

#if defined(_WIN32)
        write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
#else
        const int host_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (host_fd < 0)
        {
            write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
            return;
        }

        sockaddr_in host_addr{};
        host_addr.sin_family = AF_INET;
        host_addr.sin_port = write_network_u16(host_port);
        host_addr.sin_addr.s_addr = write_network_u32(0x7F000001U);
        if (::connect(host_fd, reinterpret_cast<sockaddr*>(&host_addr), sizeof(host_addr)) != 0)
        {
            ::close(host_fd);
            write_linux_syscall_result(c, -LINUX_ECONNREFUSED);
            return;
        }

        if (state->host_socket >= 0)
        {
            ::close(state->host_socket);
        }
        state->host_socket = host_fd;
        state->connected = true;
        apply_host_socket_flags(*state, *fd_entry);
        write_linux_syscall_result(c, 0);
#endif
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
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto len = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = socket_state_for_fd(fd_entry);
        if (!state || !state->connected)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        if (state->host_socket >= 0)
        {
            write_linux_syscall_result(c, linux_socket_send_guest_buffer(c, sockfd, buf_addr, len, flags));
            return;
        }

        write_linux_syscall_result(c, static_cast<int64_t>(len));
    }

    void sys_recvfrom(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto buf_addr = get_linux_syscall_argument(c.emu, 1);
        const auto len = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = socket_state_for_fd(fd_entry);
        if (!state || !state->connected)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        if (state->host_socket >= 0)
        {
            write_linux_syscall_result(c, linux_socket_recv_guest_buffer(c, sockfd, buf_addr, len, flags));
            return;
        }

        write_linux_syscall_result(c, -LINUX_EAGAIN);
    }

    void sys_sendmsg(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto msg_addr = get_linux_syscall_argument(c.emu, 1);
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        const auto* state = socket_state_for_fd(fd_entry);
        if (state != nullptr && state->host_socket >= 0)
        {
            uint64_t iov_addr{};
            uint64_t iovcnt{};
            c.emu.read_memory(msg_addr + 16, &iov_addr, sizeof(iov_addr));
            c.emu.read_memory(msg_addr + 24, &iovcnt, sizeof(iovcnt));
            write_linux_syscall_result(c, linux_socket_writev_from_guest_with_flags(c, sockfd, iov_addr, static_cast<int>(iovcnt), flags));
            return;
        }

        write_linux_syscall_result(c, -LINUX_ENOTCONN);
    }

    void sys_recvmsg(const linux_syscall_context& c)
    {
        const auto sockfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto msg_addr = get_linux_syscall_argument(c.emu, 1);
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        const auto* state = socket_state_for_fd(fd_entry);
        if (state != nullptr && state->host_socket >= 0)
        {
            uint64_t iov_addr{};
            uint64_t iovcnt{};
            c.emu.read_memory(msg_addr + 16, &iov_addr, sizeof(iov_addr));
            c.emu.read_memory(msg_addr + 24, &iovcnt, sizeof(iovcnt));
            const auto result = linux_socket_readv_to_guest_with_flags(c, sockfd, iov_addr, static_cast<int>(iovcnt), flags);
            if (result >= 0)
            {
                uint32_t msg_flags = 0;
                c.emu.write_memory(msg_addr + 48, &msg_flags, sizeof(msg_flags));
            }
            write_linux_syscall_result(c, result);
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

        auto* state = socket_state_for_fd(fd_entry);
        if (state)
        {
#if !defined(_WIN32)
            if (state->host_socket >= 0)
            {
                ::close(state->host_socket);
                state->host_socket = -1;
            }
#endif
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

        auto* state = socket_state_for_fd(fd_entry);
        if (!state)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        // Read the address family
        uint16_t family{};
        c.emu.read_memory(addr_ptr, &family, sizeof(family));

        if (family == LINUX_AF_INET && addrlen >= sizeof(linux_sockaddr_in))
        {
            linux_sockaddr_in sin{};
            c.emu.read_memory(addr_ptr, &sin, sizeof(sin));
            state->bound_port = sin.sin_port;
            state->bound_addr = sin.sin_addr;
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

        if (auto* state = socket_state_for_fd(fd_entry))
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

        auto* state = socket_state_for_fd(fd_entry);
        if (!state)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        int val = 0;
        if (optval_addr && optlen >= sizeof(int))
        {
            c.emu.read_memory(optval_addr, &val, sizeof(val));
        }

        if (level == LINUX_SOL_SOCKET)
        {
            switch (optname)
            {
            case LINUX_SO_REUSEADDR:
                state->so_reuseaddr = val;
                break;
            case LINUX_SO_REUSEPORT:
                state->so_reuseport = val;
                break;
            case LINUX_SO_KEEPALIVE:
                state->so_keepalive = val;
                break;
            case LINUX_SO_SNDBUF:
                state->so_sndbuf = val;
                break;
            case LINUX_SO_RCVBUF:
                state->so_rcvbuf = val;
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

        auto* state = socket_state_for_fd(fd_entry);
        if (!state)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        int val = 0;

        if (level == LINUX_SOL_SOCKET)
        {
            switch (optname)
            {
            case LINUX_SO_REUSEADDR:
                val = state->so_reuseaddr;
                break;
            case LINUX_SO_REUSEPORT:
                val = state->so_reuseport;
                break;
            case LINUX_SO_KEEPALIVE:
                val = state->so_keepalive;
                break;
            case LINUX_SO_SNDBUF:
                val = state->so_sndbuf;
                break;
            case LINUX_SO_RCVBUF:
                val = state->so_rcvbuf;
                break;
            case LINUX_SO_TYPE:
                val = state->type;
                break;
            case LINUX_SO_ERROR:
                val = state->so_error;
                state->so_error = 0; // Clear after reading
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

        if (domain != LINUX_AF_UNIX)
        {
            write_linux_syscall_result(c, -LINUX_EAFNOSUPPORT);
            return;
        }

        write_linux_syscall_result(c, -LINUX_EOPNOTSUPP);
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

        auto* state = socket_state_for_fd(fd_entry);
        if (!state)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        if (state->domain == LINUX_AF_INET)
        {
            linux_sockaddr_in sin{};
            sin.sin_family = LINUX_AF_INET;
            sin.sin_port = state->bound_port;
            sin.sin_addr = state->bound_addr;

            c.emu.write_memory(addr_ptr, &sin, sizeof(sin));

            uint32_t len = sizeof(linux_sockaddr_in);
            c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
        }
        else
        {
            // Return a minimal sockaddr with just the family
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

        auto* fd_entry = c.proc.fds.get(sockfd);
        if (!fd_entry || fd_entry->type != fd_type::socket)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        auto* state = socket_state_for_fd(fd_entry);
        if (!state || !state->connected)
        {
            write_linux_syscall_result(c, -LINUX_ENOTCONN);
            return;
        }

        // Return a synthetic peer address
        const auto addr_ptr = get_linux_syscall_argument(c.emu, 1);
        const auto addrlen_ptr = get_linux_syscall_argument(c.emu, 2);

        if (state->domain == LINUX_AF_INET)
        {
            linux_sockaddr_in sin{};
            sin.sin_family = LINUX_AF_INET;
            sin.sin_port = 0;
            sin.sin_addr = write_network_u32(0x7F000001U);

            c.emu.write_memory(addr_ptr, &sin, sizeof(sin));

            uint32_t len = sizeof(linux_sockaddr_in);
            c.emu.write_memory(addrlen_ptr, &len, sizeof(len));
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
