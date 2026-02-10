#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <fcntl.h>
#include <unistd.h>

using namespace linux_errno;

// --- Phase 4b: I/O syscalls (pipe, eventfd) ---

void sys_pipe(const linux_syscall_context& c)
{
    const auto pipefd_addr = get_linux_syscall_argument(c.emu, 0);

    int host_pipe[2] = {-1, -1};
    if (::pipe(host_pipe) != 0)
    {
        write_linux_syscall_result(c, -LINUX_EMFILE);
        return;
    }

    auto* read_stream = fdopen(host_pipe[0], "rb");
    auto* write_stream = fdopen(host_pipe[1], "wb");

    if (!read_stream || !write_stream)
    {
        if (read_stream)
        {
            fclose(read_stream);
        }
        else if (host_pipe[0] >= 0)
        {
            close(host_pipe[0]);
        }

        if (write_stream)
        {
            fclose(write_stream);
        }
        else if (host_pipe[1] >= 0)
        {
            close(host_pipe[1]);
        }

        write_linux_syscall_result(c, -LINUX_EMFILE);
        return;
    }

    setvbuf(read_stream, nullptr, _IONBF, 0);
    setvbuf(write_stream, nullptr, _IONBF, 0);

    linux_fd read_end{};
    read_end.type = fd_type::pipe_read;
    read_end.handle = read_stream;

    linux_fd write_end{};
    write_end.type = fd_type::pipe_write;
    write_end.handle = write_stream;

    const auto read_fd = c.proc.fds.allocate(std::move(read_end));
    const auto write_fd = c.proc.fds.allocate(std::move(write_end));

    int32_t fds[2] = {read_fd, write_fd};
    c.emu.write_memory(pipefd_addr, fds, sizeof(fds));

    write_linux_syscall_result(c, 0);
}

void sys_pipe2(const linux_syscall_context& c)
{
    const auto pipefd_addr = get_linux_syscall_argument(c.emu, 0);
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 1));

    constexpr int LINUX_O_CLOEXEC = 02000000;
    constexpr int LINUX_O_NONBLOCK = 04000;

    int host_pipe[2] = {-1, -1};
    if (::pipe(host_pipe) != 0)
    {
        write_linux_syscall_result(c, -LINUX_EMFILE);
        return;
    }

    const bool nonblock = (flags & LINUX_O_NONBLOCK) != 0;
    const bool cloexec = (flags & LINUX_O_CLOEXEC) != 0;

    if (nonblock)
    {
        for (int fd : host_pipe)
        {
            const auto current = fcntl(fd, F_GETFL, 0);
            if (current >= 0)
            {
                (void)fcntl(fd, F_SETFL, current | O_NONBLOCK);
            }
        }
    }

    if (cloexec)
    {
        for (int fd : host_pipe)
        {
            const auto current = fcntl(fd, F_GETFD, 0);
            if (current >= 0)
            {
                (void)fcntl(fd, F_SETFD, current | FD_CLOEXEC);
            }
        }
    }

    auto* read_stream = fdopen(host_pipe[0], "rb");
    auto* write_stream = fdopen(host_pipe[1], "wb");

    if (!read_stream || !write_stream)
    {
        if (read_stream)
        {
            fclose(read_stream);
        }
        else if (host_pipe[0] >= 0)
        {
            close(host_pipe[0]);
        }

        if (write_stream)
        {
            fclose(write_stream);
        }
        else if (host_pipe[1] >= 0)
        {
            close(host_pipe[1]);
        }

        write_linux_syscall_result(c, -LINUX_EMFILE);
        return;
    }

    setvbuf(read_stream, nullptr, _IONBF, 0);
    setvbuf(write_stream, nullptr, _IONBF, 0);

    linux_fd read_end{};
    read_end.type = fd_type::pipe_read;
    read_end.handle = read_stream;
    read_end.close_on_exec = cloexec;
    read_end.flags = flags & LINUX_O_NONBLOCK;

    linux_fd write_end{};
    write_end.type = fd_type::pipe_write;
    write_end.handle = write_stream;
    write_end.close_on_exec = cloexec;
    write_end.flags = flags & LINUX_O_NONBLOCK;

    const auto read_fd = c.proc.fds.allocate(std::move(read_end));
    const auto write_fd = c.proc.fds.allocate(std::move(write_end));

    int32_t fds[2] = {read_fd, write_fd};
    c.emu.write_memory(pipefd_addr, fds, sizeof(fds));

    write_linux_syscall_result(c, 0);
}

void sys_eventfd(const linux_syscall_context& c)
{
    // eventfd(unsigned int initval) — simplified stub
    // We create an fd entry but don't implement full eventfd semantics
    (void)get_linux_syscall_argument(c.emu, 0); // initval

    linux_fd efd{};
    efd.type = fd_type::eventfd;

    const auto fd_num = c.proc.fds.allocate(std::move(efd));
    write_linux_syscall_result(c, fd_num);
}

void sys_eventfd2(const linux_syscall_context& c)
{
    // eventfd2(unsigned int initval, int flags) — simplified stub
    (void)get_linux_syscall_argument(c.emu, 0); // initval
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 1));

    constexpr int LINUX_O_CLOEXEC = 02000000;

    linux_fd efd{};
    efd.type = fd_type::eventfd;
    efd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

    const auto fd_num = c.proc.fds.allocate(std::move(efd));
    write_linux_syscall_result(c, fd_num);
}

// --- Phase 4c: I/O multiplexing syscalls ---

namespace
{
    // Epoll constants
    constexpr int EPOLL_CTL_ADD = 1;
    constexpr int EPOLL_CTL_DEL = 2;
    constexpr int EPOLL_CTL_MOD = 3;

    // Epoll event flags
    constexpr uint32_t EPOLLIN = 0x001;
    constexpr uint32_t EPOLLOUT = 0x004;

    // Poll event flags
    constexpr int16_t POLLIN = 0x0001;
    constexpr int16_t POLLOUT = 0x0004;
    constexpr int16_t POLLNVAL = 0x0020;

#pragma pack(push, 1)
    struct linux_epoll_event
    {
        uint32_t events;
        uint64_t data;
    };
#pragma pack(pop)

    static_assert(sizeof(linux_epoll_event) == 12);

    struct epoll_entry
    {
        int fd;
        uint32_t events;
        uint64_t data;
    };

    // Epoll instance tracking
    std::map<int, std::vector<epoll_entry>> g_epoll_instances;
}

void sys_epoll_create1(const linux_syscall_context& c)
{
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    constexpr int LINUX_O_CLOEXEC = 02000000;

    linux_fd efd{};
    efd.type = fd_type::file; // epoll fd
    efd.close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;

    const auto fd_num = c.proc.fds.allocate(std::move(efd));
    g_epoll_instances[fd_num] = {};

    write_linux_syscall_result(c, fd_num);
}

void sys_epoll_ctl(const linux_syscall_context& c)
{
    const auto epfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto op = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
    const auto event_addr = get_linux_syscall_argument(c.emu, 3);

    auto it = g_epoll_instances.find(epfd);
    if (it == g_epoll_instances.end())
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    linux_epoll_event ev{};
    if (event_addr != 0)
    {
        c.emu.read_memory(event_addr, &ev, sizeof(ev));
    }

    switch (op)
    {
    case EPOLL_CTL_ADD: {
        epoll_entry entry{};
        entry.fd = fd;
        entry.events = ev.events;
        entry.data = ev.data;
        it->second.push_back(entry);
        write_linux_syscall_result(c, 0);
        break;
    }
    case EPOLL_CTL_DEL: {
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(), [fd](const epoll_entry& e) { return e.fd == fd; }), entries.end());
        write_linux_syscall_result(c, 0);
        break;
    }
    case EPOLL_CTL_MOD: {
        for (auto& entry : it->second)
        {
            if (entry.fd == fd)
            {
                entry.events = ev.events;
                entry.data = ev.data;
                break;
            }
        }
        write_linux_syscall_result(c, 0);
        break;
    }
    default:
        write_linux_syscall_result(c, -LINUX_EINVAL);
        break;
    }
}

void sys_epoll_wait(const linux_syscall_context& c)
{
    const auto epfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto events_addr = get_linux_syscall_argument(c.emu, 1);
    const auto maxevents = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
    const auto timeout = static_cast<int>(get_linux_syscall_argument(c.emu, 3));

    (void)timeout;

    auto it = g_epoll_instances.find(epfd);
    if (it == g_epoll_instances.end())
    {
        write_linux_syscall_result(c, -LINUX_EBADF);
        return;
    }

    // Simulate: check each registered fd for readiness
    // For emulation purposes, we report writable fds as ready (common for non-blocking sockets)
    int count = 0;
    for (const auto& entry : it->second)
    {
        if (count >= maxevents)
        {
            break;
        }

        auto* fd_entry = c.proc.fds.get(entry.fd);
        if (!fd_entry)
        {
            continue;
        }

        uint32_t ready_events = 0;

        // Files/pipes with data: check if readable
        if ((entry.events & EPOLLIN) && fd_entry->handle)
        {
            // Check if there's data available
            if (fd_entry->type == fd_type::file && fd_entry->handle)
            {
                const auto pos = ftell(fd_entry->handle);
                fseek(fd_entry->handle, 0, SEEK_END);
                const auto end = ftell(fd_entry->handle);
                fseek(fd_entry->handle, pos, SEEK_SET);
                if (pos < end)
                {
                    ready_events |= EPOLLIN;
                }
            }
        }

        // Most fds are writable
        if (entry.events & EPOLLOUT)
        {
            ready_events |= EPOLLOUT;
        }

        if (ready_events != 0)
        {
            linux_epoll_event out_ev{};
            out_ev.events = ready_events;
            out_ev.data = entry.data;
            c.emu.write_memory(events_addr + static_cast<uint64_t>(count) * sizeof(linux_epoll_event), &out_ev, sizeof(out_ev));
            ++count;
        }
    }

    // If nothing is ready and timeout is 0 (non-blocking), return 0
    // If nothing is ready and timeout > 0, we'd need to block — return 0 to avoid deadlock
    write_linux_syscall_result(c, count);
}

void sys_epoll_pwait(const linux_syscall_context& c)
{
    // Same as epoll_wait but with a signal mask argument (arg 4, arg 5)
    // We ignore the signal mask and delegate to epoll_wait logic
    sys_epoll_wait(c);
}

void sys_poll(const linux_syscall_context& c)
{
    const auto fds_addr = get_linux_syscall_argument(c.emu, 0);
    const auto nfds = static_cast<uint32_t>(get_linux_syscall_argument(c.emu, 1));
    const auto timeout = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    (void)timeout;

#pragma pack(push, 1)
    struct linux_pollfd
    {
        int32_t fd;
        int16_t events;
        int16_t revents;
    };
#pragma pack(pop)

    static_assert(sizeof(linux_pollfd) == 8);

    int ready_count = 0;

    for (uint32_t i = 0; i < nfds; ++i)
    {
        const auto entry_addr = fds_addr + static_cast<uint64_t>(i) * sizeof(linux_pollfd);
        linux_pollfd pfd{};
        c.emu.read_memory(entry_addr, &pfd, sizeof(pfd));

        pfd.revents = 0;

        if (pfd.fd < 0)
        {
            // Negative fd: ignore, leave revents as 0
            c.emu.write_memory(entry_addr, &pfd, sizeof(pfd));
            continue;
        }

        auto* fd_entry = c.proc.fds.get(pfd.fd);
        if (!fd_entry)
        {
            pfd.revents = POLLNVAL;
            c.emu.write_memory(entry_addr, &pfd, sizeof(pfd));
            ++ready_count;
            continue;
        }

        // Check readiness
        if (pfd.events & POLLIN)
        {
            if (fd_entry->type == fd_type::file && fd_entry->handle)
            {
                const auto pos = ftell(fd_entry->handle);
                fseek(fd_entry->handle, 0, SEEK_END);
                const auto end = ftell(fd_entry->handle);
                fseek(fd_entry->handle, pos, SEEK_SET);
                if (pos < end)
                {
                    pfd.revents |= POLLIN;
                }
            }
            // stdin: report not ready (would block)
            // pipes: could check but simplified for now
        }

        if (pfd.events & POLLOUT)
        {
            // Most fds are writable
            if (fd_entry->type == fd_type::file || fd_entry->type == fd_type::pipe_write || fd_entry->type == fd_type::socket)
            {
                pfd.revents |= POLLOUT;
            }
        }

        if (pfd.revents != 0)
        {
            ++ready_count;
        }

        c.emu.write_memory(entry_addr, &pfd, sizeof(pfd));
    }

    write_linux_syscall_result(c, ready_count);
}

void sys_ppoll(const linux_syscall_context& c)
{
    // ppoll is poll with a timespec timeout and signal mask
    // We ignore the signal mask and treat it as poll
    sys_poll(c);
}

void sys_select(const linux_syscall_context& c)
{
    const auto nfds = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
    const auto readfds_addr = get_linux_syscall_argument(c.emu, 1);
    const auto writefds_addr = get_linux_syscall_argument(c.emu, 2);
    const auto exceptfds_addr = get_linux_syscall_argument(c.emu, 3);
    // timeout is arg 4

    // fd_set is 128 bytes (1024 bits) on Linux
    constexpr size_t FD_SET_SIZE = 128;

    auto read_fd_set = [&](uint64_t addr, std::vector<uint8_t>& bits) {
        bits.resize(FD_SET_SIZE, 0);
        if (addr)
        {
            c.emu.read_memory(addr, bits.data(), FD_SET_SIZE);
        }
    };

    auto write_fd_set = [&](uint64_t addr, const std::vector<uint8_t>& bits) {
        if (addr)
        {
            c.emu.write_memory(addr, bits.data(), FD_SET_SIZE);
        }
    };

    auto fd_isset = [](int fd, const std::vector<uint8_t>& bits) -> bool {
        if (fd < 0 || static_cast<size_t>(fd / 8) >= bits.size())
        {
            return false;
        }
        return (bits[fd / 8] & (1 << (fd % 8))) != 0;
    };

    auto fd_set_bit = [](int fd, std::vector<uint8_t>& bits) {
        if (fd >= 0 && static_cast<size_t>(fd / 8) < bits.size())
        {
            bits[fd / 8] |= static_cast<uint8_t>(1 << (fd % 8));
        }
    };

    std::vector<uint8_t> read_bits, write_bits, except_bits;
    read_fd_set(readfds_addr, read_bits);
    read_fd_set(writefds_addr, write_bits);
    read_fd_set(exceptfds_addr, except_bits);

    // Clear output sets
    std::vector<uint8_t> out_read(FD_SET_SIZE, 0);
    std::vector<uint8_t> out_write(FD_SET_SIZE, 0);
    std::vector<uint8_t> out_except(FD_SET_SIZE, 0);

    int ready_count = 0;

    for (int fd = 0; fd < nfds; ++fd)
    {
        const bool want_read = readfds_addr && fd_isset(fd, read_bits);
        const bool want_write = writefds_addr && fd_isset(fd, write_bits);
        const bool want_except = exceptfds_addr && fd_isset(fd, except_bits);

        if (!want_read && !want_write && !want_except)
        {
            continue;
        }

        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry)
        {
            continue;
        }

        if (want_write)
        {
            // Most fds are writable
            fd_set_bit(fd, out_write);
            ++ready_count;
        }

        if (want_read && fd_entry->handle)
        {
            if (fd_entry->type == fd_type::file)
            {
                const auto pos = ftell(fd_entry->handle);
                fseek(fd_entry->handle, 0, SEEK_END);
                const auto end = ftell(fd_entry->handle);
                fseek(fd_entry->handle, pos, SEEK_SET);
                if (pos < end)
                {
                    fd_set_bit(fd, out_read);
                    ++ready_count;
                }
            }
        }

        // No exceptions
        (void)want_except;
    }

    write_fd_set(readfds_addr, out_read);
    write_fd_set(writefds_addr, out_write);
    write_fd_set(exceptfds_addr, out_except);

    write_linux_syscall_result(c, ready_count);
}

void sys_pselect6(const linux_syscall_context& c)
{
    // pselect6 is select with a timespec and signal mask
    // We ignore the signal mask and delegate to select logic
    sys_select(c);
}
