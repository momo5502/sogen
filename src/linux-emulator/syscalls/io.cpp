#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <posix_pipe.hpp>

#include <array>
#include <algorithm>
#include <fcntl.h>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

    namespace
    {
        constexpr int EPOLL_CTL_ADD = 1;
        constexpr int EPOLL_CTL_DEL = 2;
        constexpr int EPOLL_CTL_MOD = 3;

        constexpr uint32_t EPOLLIN = 0x001;
        constexpr uint32_t EPOLLOUT = 0x004;

        constexpr int16_t POLLIN = 0x0001;
        constexpr int16_t POLLOUT = 0x0004;
        constexpr int16_t POLLNVAL = 0x0020;

        void create_pipe(const linux_syscall_context& c, const uint64_t pipefd_addr, const int flags)
        {
            constexpr int LINUX_O_CLOEXEC = 02000000;
            constexpr int LINUX_O_NONBLOCK = 04000;

            const auto result = posix_create_pipe(c.proc.fds, flags & LINUX_O_NONBLOCK, (flags & LINUX_O_CLOEXEC) != 0);
            if (const auto* error = std::get_if<posix_errno>(&result))
            {
                write_linux_syscall_result(c, -linux_errno_of(*error));
                return;
            }

            const auto& created = std::get<posix_pipe>(result);
            const std::array<int32_t, 2> descriptors{created.read_fd, created.write_fd};
            if (!c.emu_ref.memory.is_writable_range(pipefd_addr, sizeof(descriptors)) ||
                !c.emu_ref.memory.try_write_memory(pipefd_addr, descriptors.data(), sizeof(descriptors)))
            {
                c.proc.fds.close(created.read_fd);
                c.proc.fds.close(created.write_fd);
                write_linux_syscall_result(c, -LINUX_EFAULT);
                return;
            }

            write_linux_syscall_result(c, 0);
        }

        bool host_file_has_remaining_data(FILE* handle)
        {
            if (!handle)
            {
                return false;
            }

            const auto pos = ftell(handle);
            if (pos < 0)
            {
                return true;
            }

            if (fseek(handle, 0, SEEK_END) != 0)
            {
                return true;
            }

            const auto end = ftell(handle);
            (void)fseek(handle, pos, SEEK_SET);
            return end < 0 || pos < end;
        }

        bool host_pipe_has_readable_data(FILE* handle)
        {
            if (!handle)
            {
                return false;
            }

#if defined(_WIN32)
            auto* const os_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(handle)));
            if (os_handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            DWORD available = 0;
            if (PeekNamedPipe(os_handle, nullptr, 0, nullptr, &available, nullptr) != 0)
            {
                return available > 0;
            }

            return GetLastError() == ERROR_BROKEN_PIPE;
#else
            const auto fd = fileno(handle);
            if (fd < 0 || fd >= FD_SETSIZE)
            {
                return false;
            }

            fd_set read_fds{};
            FD_ZERO(&read_fds);
            FD_SET(fd, &read_fds);
            timeval timeout{};
            const auto result = ::select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
            return result > 0 && FD_ISSET(fd, &read_fds);
#endif
        }

        bool epoll_has_ready_entries(linux_process_context& proc, const posix_fd& epoll, std::vector<int>& epoll_stack);

        bool fd_has_readable_data(linux_process_context& proc, const int fd, const posix_fd& fd_entry, std::vector<int>& epoll_stack)
        {
            if (fd_entry.type == fd_type::memory_file)
            {
                return fd_entry.memory_file && fd_entry.memory_file->offset < fd_entry.memory_file->content.size();
            }

            if (fd_entry.type == fd_type::file)
            {
                return host_file_has_remaining_data(fd_entry.handle);
            }

            if (fd_entry.type == fd_type::pipe_read)
            {
                return host_pipe_has_readable_data(fd_entry.handle);
            }

            if (fd_entry.type != fd_type::epoll)
            {
                return false;
            }

            if (std::ranges::find(epoll_stack, fd) != epoll_stack.end())
            {
                return false;
            }

            epoll_stack.push_back(fd);
            const auto has_ready_entries = epoll_has_ready_entries(proc, fd_entry, epoll_stack);
            epoll_stack.pop_back();
            return has_ready_entries;
        }

        bool fd_accepts_write_ready(const posix_fd& fd)
        {
            return fd.type == fd_type::file || fd.type == fd_type::pipe_write || fd.type == fd_type::socket;
        }

        uint32_t ready_events_for_entry(linux_process_context& proc, const posix_epoll_entry& entry, std::vector<int>& epoll_stack)
        {
            auto* fd_entry = proc.fds.get(entry.fd);
            if (!fd_entry)
            {
                return 0;
            }

            uint32_t ready_events = 0;
            if ((entry.events & EPOLLIN) && fd_has_readable_data(proc, entry.fd, *fd_entry, epoll_stack))
            {
                ready_events |= EPOLLIN;
            }

            if ((entry.events & EPOLLOUT) && fd_accepts_write_ready(*fd_entry))
            {
                ready_events |= EPOLLOUT;
            }

            return ready_events;
        }

        bool epoll_has_ready_entries(linux_process_context& proc, const posix_fd& epoll, std::vector<int>& epoll_stack)
        {
            return std::ranges::any_of(epoll.epoll_entries, [&proc, &epoll_stack](const posix_epoll_entry& entry) {
                return ready_events_for_entry(proc, entry, epoll_stack) != 0;
            });
        }
    }

    void sys_pipe(const linux_syscall_context& c)
    {
        create_pipe(c, get_linux_syscall_argument(c.emu, 0), 0);
    }

    void sys_pipe2(const linux_syscall_context& c)
    {
        create_pipe(c, get_linux_syscall_argument(c.emu, 0), static_cast<int>(get_linux_syscall_argument(c.emu, 1)));
    }

    void sys_eventfd(const linux_syscall_context& c)
    {
        // eventfd(unsigned int initval) — simplified stub
        // We create an fd entry but don't implement full eventfd semantics
        (void)get_linux_syscall_argument(c.emu, 0); // initval

        posix_fd efd{};
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

        posix_fd efd{};
        efd.type = fd_type::eventfd;

        const auto fd_num = c.proc.fds.allocate(std::move(efd), (flags & LINUX_O_CLOEXEC) != 0);
        write_linux_syscall_result(c, fd_num);
    }

    namespace
    {

#pragma pack(push, 1)

        struct linux_epoll_event
        {
            uint32_t events;
            uint64_t data;
        };

#pragma pack(pop)

        static_assert(sizeof(linux_epoll_event) == 12);

    }

    void sys_epoll_create1(const linux_syscall_context& c)
    {
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        constexpr int LINUX_O_CLOEXEC = 02000000;

        posix_fd efd{};
        efd.type = fd_type::epoll;

        const auto fd_num = c.proc.fds.allocate(std::move(efd), (flags & LINUX_O_CLOEXEC) != 0);

        write_linux_syscall_result(c, fd_num);
    }

    void sys_epoll_ctl(const linux_syscall_context& c)
    {
        const auto epfd = static_cast<int>(get_linux_syscall_argument(c.emu, 0));
        const auto op = static_cast<int>(get_linux_syscall_argument(c.emu, 1));
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
        const auto event_addr = get_linux_syscall_argument(c.emu, 3);

        auto* epoll_fd = c.proc.fds.get(epfd);
        if (!epoll_fd || epoll_fd->type != fd_type::epoll)
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
            epoll_fd->epoll_entries.push_back({.fd = fd, .events = ev.events, .data = ev.data});
            write_linux_syscall_result(c, 0);
            break;
        }
        case EPOLL_CTL_DEL: {
            std::erase_if(epoll_fd->epoll_entries, [fd](const posix_epoll_entry& e) { return e.fd == fd; });
            write_linux_syscall_result(c, 0);
            break;
        }
        case EPOLL_CTL_MOD: {
            for (auto& entry : epoll_fd->epoll_entries)
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

        auto* epoll_fd = c.proc.fds.get(epfd);
        if (!epoll_fd || epoll_fd->type != fd_type::epoll)
        {
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        int count = 0;
        std::vector<int> epoll_stack{epfd};
        for (const auto& entry : epoll_fd->epoll_entries)
        {
            if (count >= maxevents)
            {
                break;
            }

            const auto ready_events = ready_events_for_entry(c.proc, entry, epoll_stack);

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

            std::vector<int> epoll_stack{};
            if ((pfd.events & POLLIN) && fd_has_readable_data(c.proc, pfd.fd, *fd_entry, epoll_stack))
            {
                pfd.revents |= POLLIN;
            }

            if ((pfd.events & POLLOUT) && fd_accepts_write_ready(*fd_entry))
            {
                pfd.revents |= POLLOUT;
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
            return (bits.at(static_cast<size_t>(fd / 8)) & (1 << (fd % 8))) != 0;
        };

        auto fd_set_bit = [](int fd, std::vector<uint8_t>& bits) {
            if (fd >= 0 && static_cast<size_t>(fd / 8) < bits.size())
            {
                bits.at(static_cast<size_t>(fd / 8)) |= static_cast<uint8_t>(1 << (fd % 8));
            }
        };

        std::vector<uint8_t> read_bits;
        std::vector<uint8_t> write_bits;
        std::vector<uint8_t> except_bits;
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

            if (want_write && fd_accepts_write_ready(*fd_entry))
            {
                fd_set_bit(fd, out_write);
                ++ready_count;
            }
            std::vector<int> epoll_stack{};
            if (want_read && fd_has_readable_data(c.proc, fd, *fd_entry, epoll_stack))
            {
                fd_set_bit(fd, out_read);
                ++ready_count;
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

    // NOLINTEND(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

} // namespace sogen
