#include "../std_include.hpp"

#include <cstring>

#include <ranges>
#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <limits>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;
    using namespace macos_fcntl;
    using namespace macos_guard;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        // Darwin refuses a read or write of more than INT_MAX bytes with EINVAL. Enforcing it here also
        // keeps the staging buffer below bounded by a limit the guest cannot inflate into a host
        // allocation failure.
        constexpr uint64_t MACOS_IO_MAX_TRANSFER = 0x7FFFFFFFULL;
        constexpr uint64_t MACOS_IO_CHUNK_SIZE = 0x10000;
        constexpr int64_t MACOS_IOV_MAX = 1024;

        int seek_host_file(FILE* handle, const int64_t offset, const int origin)
        {
#if defined(_WIN32)
            return _fseeki64(handle, offset, origin);
#else
            return fseeko(handle, static_cast<off_t>(offset), origin);
#endif
        }

        int host_seek_origin(const int32_t whence)
        {
            switch (whence)
            {
            case MACOS_SEEK_CUR:
                return SEEK_CUR;
            case MACOS_SEEK_END:
                return SEEK_END;
            default:
                return SEEK_SET;
            }
        }

        int64_t memory_file_seek_base(const int32_t whence, const guest_memory_fd& file)
        {
            switch (whence)
            {
            case MACOS_SEEK_CUR:
                return static_cast<int64_t>(file.offset);
            case MACOS_SEEK_END:
                return static_cast<int64_t>(file.content.size());
            default:
                return 0;
            }
        }

        int64_t tell_host_file(FILE* handle)
        {
#if defined(_WIN32)
            return _ftelli64(handle);
#else
            return static_cast<int64_t>(ftello(handle));
#endif
        }

        enum class stdio_role : uint8_t
        {
            none,
            standard_input,
            standard_output,
            standard_error,
        };

        // Routing on the descriptor number would miss a duplicate: guest_fd_table::duplicate_entry copies
        // host_path and the host handle, so dup(1) yields an fd that writes straight past on_stdout into
        // the emulator's own terminal.
        stdio_role classify_stdio_role(const guest_fd& entry)
        {
            if (entry.host_path == "/dev/stdout")
            {
                return stdio_role::standard_output;
            }

            if (entry.host_path == "/dev/stderr")
            {
                return stdio_role::standard_error;
            }

            if (entry.host_path == "/dev/stdin")
            {
                return stdio_role::standard_input;
            }

            return stdio_role::none;
        }

        size_t emit_to_standard_stream(const macos_syscall_context& c, const stdio_role role, const uint8_t* data, const size_t size)
        {
            auto& callback = (role == stdio_role::standard_output) ? c.emu_ref.callbacks.on_stdout : c.emu_ref.callbacks.on_stderr;
            if (callback)
            {
                callback(std::string_view{reinterpret_cast<const char*>(data), size});
                return size;
            }

            auto* target = (role == stdio_role::standard_output) ? stdout : stderr;
            const auto written = fwrite(data, 1, size, target);
            fflush(target);
            return written;
        }

        int64_t writable_fd_error(const guest_fd& entry)
        {
            const auto role = classify_stdio_role(entry);
            if (role == stdio_role::standard_output || role == stdio_role::standard_error)
            {
                return 0;
            }

            if (role == stdio_role::standard_input || entry.type == fd_type::directory || entry.type == fd_type::pipe_read)
            {
                return MACOS_EBADF;
            }

            if (entry.read_only_mapping)
            {
                return MACOS_EACCES;
            }

            if (is_macos_null_device(entry))
            {
                return 0;
            }

            return entry.handle != nullptr ? 0 : MACOS_EBADF;
        }

        int64_t transfer_to_fd(const macos_syscall_context& c, guest_fd& entry, uint64_t address, uint64_t length, int64_t& error)
        {
            if (is_macos_null_device(entry))
            {
                return static_cast<int64_t>(std::min<uint64_t>(length, MACOS_IO_MAX_TRANSFER));
            }

            const auto role = classify_stdio_role(entry);
            int64_t transferred = 0;
            std::vector<uint8_t> chunk(static_cast<size_t>(std::min(MACOS_IO_CHUNK_SIZE, length)));

            while (length > 0)
            {
                const auto count = static_cast<size_t>(std::min<uint64_t>(chunk.size(), length));
                if (!c.emu_ref.memory.try_read_memory(address, chunk.data(), count))
                {
                    if (transferred == 0)
                    {
                        error = MACOS_EFAULT;
                    }
                    break;
                }

                const auto consumed = (role == stdio_role::none) ? fwrite(chunk.data(), 1, count, entry.handle)
                                                                 : emit_to_standard_stream(c, role, chunk.data(), count);

                transferred += static_cast<int64_t>(consumed);
                address += consumed;
                length -= consumed;

                if (consumed < count)
                {
                    break;
                }
            }

            return transferred;
        }

        int64_t transfer_from_memory_file(const macos_syscall_context& c, guest_fd& entry, const uint64_t address, const uint64_t length,
                                          int64_t& error)
        {
            auto& file = *entry.memory_file;
            if (file.offset >= file.content.size())
            {
                return 0;
            }

            const auto available = file.content.size() - file.offset;
            const auto count = static_cast<size_t>(std::min<uint64_t>(available, length));
            if (count == 0)
            {
                return 0;
            }

            if (!c.emu_ref.memory.try_write_memory(address, file.content.data() + file.offset, count))
            {
                error = MACOS_EFAULT;
                return 0;
            }

            file.offset += count;
            return static_cast<int64_t>(count);
        }

        int64_t transfer_from_fd(const macos_syscall_context& c, guest_fd& entry, uint64_t address, uint64_t length, int64_t& error)
        {
            if (entry.type == fd_type::memory_file)
            {
                if (!entry.memory_file)
                {
                    error = MACOS_EBADF;
                    return 0;
                }

                return transfer_from_memory_file(c, entry, address, length, error);
            }

            // Never touch the host's own terminal: a blocking read on it would hang the emulator, and a
            // guest that gets EOF instead simply stops asking.
            if (guest_fd_detail::is_stdio_path(entry.host_path))
            {
                return 0;
            }

            if (entry.type == fd_type::directory)
            {
                error = MACOS_EISDIR;
                return 0;
            }

            if (entry.type == fd_type::pipe_write || entry.handle == nullptr)
            {
                error = MACOS_EBADF;
                return 0;
            }

            int64_t transferred = 0;
            std::vector<uint8_t> chunk(static_cast<size_t>(std::min(MACOS_IO_CHUNK_SIZE, length)));

            while (length > 0)
            {
                const auto count = static_cast<size_t>(std::min<uint64_t>(chunk.size(), length));
                const auto received = fread(chunk.data(), 1, count, entry.handle);
                if (received == 0)
                {
                    break;
                }

                if (!c.emu_ref.memory.try_write_memory(address, chunk.data(), received))
                {
                    if (transferred == 0)
                    {
                        error = MACOS_EFAULT;
                    }
                    break;
                }

                transferred += static_cast<int64_t>(received);
                address += received;
                length -= received;

                if (received < count)
                {
                    break;
                }
            }

            return transferred;
        }

        bool read_iovec_array(const macos_syscall_context& c, const uint64_t address, const int64_t iovcnt,
                              std::vector<macos_iovec>& vectors, int64_t& error)
        {
            if (iovcnt < 0 || iovcnt > MACOS_IOV_MAX)
            {
                error = MACOS_EINVAL;
                return false;
            }

            vectors.resize(static_cast<size_t>(iovcnt));
            if (!vectors.empty() && !c.emu_ref.memory.try_read_memory(address, vectors.data(), vectors.size() * sizeof(macos_iovec)))
            {
                error = MACOS_EFAULT;
                return false;
            }

            uint64_t total = 0;
            for (const auto& vector : vectors)
            {
                if (vector.iov_len > MACOS_IO_MAX_TRANSFER || total > MACOS_IO_MAX_TRANSFER - vector.iov_len)
                {
                    error = MACOS_EINVAL;
                    return false;
                }

                total += vector.iov_len;
            }

            return true;
        }

        int read_syscall_fd(const macos_syscall_context& c, const size_t index)
        {
            return static_cast<int>(static_cast<int32_t>(get_macos_syscall_argument(c, index)));
        }

        int64_t duplicate_descriptor(const macos_syscall_context& c, const int fd, const int minimum, int& new_fd)
        {
            new_fd = c.proc.fds.dup_fd(fd, minimum);
            if (new_fd < 0)
            {
                return MACOS_EMFILE;
            }

            if (new_fd >= MACOS_MAX_OPEN_DESCRIPTORS)
            {
                c.proc.fds.close(new_fd);
                return MACOS_EMFILE;
            }

            return 0;
        }

        // The bodies below are shared with the guarded_* forms, which differ from the plain syscalls only
        // in naming the descriptor's guard first.
        void write_to_descriptor(const macos_syscall_context& c, const int fd, const uint64_t address, const uint64_t length)
        {
            auto* entry = c.proc.fds.get(fd);
            if (entry == nullptr)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            if (length > MACOS_IO_MAX_TRANSFER)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            const auto fd_error = writable_fd_error(*entry);
            if (fd_error != 0)
            {
                write_macos_syscall_error(c, fd_error);
                return;
            }

            int64_t error = 0;
            const auto written = transfer_to_fd(c, *entry, address, length, error);
            if (error != 0)
            {
                write_macos_syscall_error(c, error);
                return;
            }

            write_macos_syscall_result(c, written);
        }

        // pwrite leaves the file offset where it found it, which is why a caller reaches for it at all:
        // SQLite keeps one descriptor per database and writes many offsets through it.
        void write_to_descriptor_at(const macos_syscall_context& c, const int fd, const uint64_t address, const uint64_t length,
                                    const int64_t offset)
        {
            auto* entry = c.proc.fds.get(fd);
            if (entry == nullptr)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            if (length > MACOS_IO_MAX_TRANSFER || offset < 0)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            if (entry->type == fd_type::directory || entry->type == fd_type::pipe_read || entry->type == fd_type::pipe_write ||
                classify_stdio_role(*entry) != stdio_role::none)
            {
                write_macos_syscall_error(c, MACOS_ESPIPE);
                return;
            }

            const auto fd_error = writable_fd_error(*entry);
            if (fd_error != 0)
            {
                write_macos_syscall_error(c, fd_error);
                return;
            }

            if (is_macos_null_device(*entry))
            {
                write_macos_syscall_result(c, static_cast<int64_t>(length));
                return;
            }

            if (entry->handle == nullptr)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            const auto saved = tell_host_file(entry->handle);
            if (saved < 0 || seek_host_file(entry->handle, offset, SEEK_SET) != 0)
            {
                write_macos_syscall_error(c, MACOS_ESPIPE);
                return;
            }

            int64_t error = 0;
            const auto written = transfer_to_fd(c, *entry, address, length, error);

            if (seek_host_file(entry->handle, saved, SEEK_SET) != 0)
            {
                write_macos_syscall_error(c, MACOS_EIO);
                return;
            }

            if (error != 0)
            {
                write_macos_syscall_error(c, error);
                return;
            }

            write_macos_syscall_result(c, written);
        }

        constexpr uint64_t lock_range_end(const uint64_t start, const uint64_t length)
        {
            return length == 0 ? std::numeric_limits<uint64_t>::max() : start + length;
        }

        constexpr uint64_t lock_range_length(const uint64_t start, const uint64_t end)
        {
            return end == std::numeric_limits<uint64_t>::max() ? 0 : end - start;
        }

        bool lock_ranges_overlap(const macos_file_lock& held, const uint64_t start, const uint64_t end)
        {
            return held.start < end && start < lock_range_end(held.start, held.length);
        }

        int64_t resolve_lock_start(const guest_fd& entry, const macos_flock& request, uint64_t& start)
        {
            int64_t base = 0;

            switch (request.l_whence)
            {
            case MACOS_SEEK_SET:
                break;

            case MACOS_SEEK_CUR:
                base = tell_host_file(entry.handle);
                break;

            case MACOS_SEEK_END: {
                std::error_code error{};
                const auto size = std::filesystem::file_size(std::filesystem::path{entry.host_path}, error);
                if (error)
                {
                    return MACOS_EINVAL;
                }

                base = static_cast<int64_t>(size);
                break;
            }

            default:
                return MACOS_EINVAL;
            }

            if (base < 0 || request.l_start < -base)
            {
                return MACOS_EINVAL;
            }

            start = static_cast<uint64_t>(base + request.l_start);
            return 0;
        }

        // The whole fcntl locking family. A guest that locks a byte range is coordinating with another
        // opener of the file, and the only openers sogen has are this process's own descriptors, so a
        // request conflicts exactly when some *other* descriptor holds an overlapping range and at least
        // one of the two is exclusive.
        void file_lock_command(const macos_syscall_context& c, const int fd, const guest_fd& entry, const int32_t command,
                               const uint64_t argument)
        {
            if (entry.type != fd_type::file || entry.handle == nullptr)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            macos_flock request{};
            if (!c.emu_ref.memory.try_read_memory(argument, &request, sizeof(request)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            // A negative l_len names the range *below* l_start. Nothing sogen has seen uses it, and
            // guessing at the sign convention would be worse than saying so.
            if (request.l_len < 0 ||
                (request.l_type != MACOS_F_RDLCK && request.l_type != MACOS_F_WRLCK && request.l_type != MACOS_F_UNLCK))
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            uint64_t start = 0;
            if (const auto error = resolve_lock_start(entry, request, start); error != 0)
            {
                write_macos_syscall_error(c, error);
                return;
            }

            const auto length = static_cast<uint64_t>(request.l_len);
            const auto end = lock_range_end(start, length);
            const auto exclusive = request.l_type == MACOS_F_WRLCK;

            auto& locks = c.proc.file_locks;
            const auto conflicting = std::ranges::find_if(locks, [&](const macos_file_lock& held) {
                return held.fd != fd && held.path == entry.guest_path && (held.exclusive || exclusive) &&
                       lock_ranges_overlap(held, start, end);
            });

            if (command == MACOS_F_GETLK || command == MACOS_F_OFD_GETLK)
            {
                if (conflicting == locks.end())
                {
                    request.l_type = MACOS_F_UNLCK;
                }
                else
                {
                    request.l_type = conflicting->exclusive ? MACOS_F_WRLCK : MACOS_F_RDLCK;
                    request.l_whence = MACOS_SEEK_SET;
                    request.l_start = static_cast<int64_t>(conflicting->start);
                    request.l_len = static_cast<int64_t>(conflicting->length);

                    // An OFD lock has no owning process, and xnu reports -1 for one. A plain F_GETLK
                    // would name the holder, which here can only ever be this process.
                    request.l_pid = command == MACOS_F_OFD_GETLK ? -1 : static_cast<int32_t>(c.proc.pid);
                }

                if (!c.emu_ref.memory.try_write_memory(argument, &request, sizeof(request)))
                {
                    write_macos_syscall_error(c, MACOS_EFAULT);
                    return;
                }

                write_macos_syscall_result(c, 0);
                return;
            }

            if (request.l_type != MACOS_F_UNLCK && conflicting != locks.end())
            {
                // F_SETLKW asks to wait for the holder. The holder is another descriptor of the only
                // process there is, and nothing can run to release it while this call blocks, so waiting
                // would be a deadlock rather than a delay; EAGAIN lets the caller report a busy file.
                if (command == MACOS_F_SETLKW || command == MACOS_F_OFD_SETLKW)
                {
                    c.emu_ref.log.warn("fd %d waits on a lock held by fd %d, which nothing in a single-process guest can release\n", fd,
                                       conflicting->fd);
                }

                write_macos_syscall_error(c, MACOS_EAGAIN);
                return;
            }

            // What this descriptor already held over the range gives way to what it is asking for now,
            // which is how a downgrade, an upgrade and an unlock are all expressed. A request that only
            // covers part of a held range leaves the rest of it standing.
            std::vector<macos_file_lock> remainders{};
            std::erase_if(locks, [&](const macos_file_lock& held) {
                if (held.fd != fd || held.path != entry.guest_path || !lock_ranges_overlap(held, start, end))
                {
                    return false;
                }

                const auto held_end = lock_range_end(held.start, held.length);
                if (held.start < start)
                {
                    remainders.push_back({.path = held.path,
                                          .fd = held.fd,
                                          .start = held.start,
                                          .length = lock_range_length(held.start, start),
                                          .exclusive = held.exclusive});
                }

                if (end < held_end)
                {
                    remainders.push_back({.path = held.path,
                                          .fd = held.fd,
                                          .start = end,
                                          .length = lock_range_length(end, held_end),
                                          .exclusive = held.exclusive});
                }

                return true;
            });

            locks.insert(locks.end(), std::make_move_iterator(remainders.begin()), std::make_move_iterator(remainders.end()));

            if (request.l_type != MACOS_F_UNLCK)
            {
                locks.push_back({.path = entry.guest_path, .fd = fd, .start = start, .length = length, .exclusive = exclusive});
            }

            write_macos_syscall_result(c, 0);
        }

        void accept_descriptor_metadata_change(const macos_syscall_context& c)
        {
            if (c.proc.fds.get(read_syscall_fd(c, 0)) == nullptr)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            write_macos_syscall_result(c, 0);
        }

        void close_descriptor(const macos_syscall_context& c, const int fd)
        {
            const auto* entry = c.proc.fds.get(fd);
            const auto is_kqueue = entry != nullptr && entry->type == fd_type::kqueue;

            if (!c.proc.fds.close(fd))
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            if (is_kqueue)
            {
                c.proc.kqueues.destroy(static_cast<uint32_t>(fd));
            }

            c.proc.forget_descriptor_state(fd);
            write_macos_syscall_result(c, 0);
        }
    }

    void sys_write(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_WRITE, "write"))
        {
            return;
        }

        write_to_descriptor(c, fd, get_macos_syscall_argument(c, 1), get_macos_syscall_argument(c, 2));
    }

    void sys_pwrite(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_WRITE, "pwrite"))
        {
            return;
        }

        write_to_descriptor_at(c, fd, get_macos_syscall_argument(c, 1), get_macos_syscall_argument(c, 2),
                               static_cast<int64_t>(get_macos_syscall_argument(c, 3)));
    }

    void sys_guarded_write_np(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_matches_argument(c, fd, get_macos_syscall_argument(c, 1), "guarded_write_np"))
        {
            return;
        }

        write_to_descriptor(c, fd, get_macos_syscall_argument(c, 2), get_macos_syscall_argument(c, 3));
    }

    void sys_guarded_pwrite_np(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_matches_argument(c, fd, get_macos_syscall_argument(c, 1), "guarded_pwrite_np"))
        {
            return;
        }

        write_to_descriptor_at(c, fd, get_macos_syscall_argument(c, 2), get_macos_syscall_argument(c, 3),
                               static_cast<int64_t>(get_macos_syscall_argument(c, 4)));
    }

    void sys_guarded_close_np(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_matches_argument(c, fd, get_macos_syscall_argument(c, 1), "guarded_close_np"))
        {
            return;
        }

        close_descriptor(c, fd);
    }

    // libSystem opens a AF_UNIX datagram socket during initialisation to reach the logging daemon.
    // Creating one costs the guest nothing and touches nothing outside it -- the connect that follows
    // finds no daemon in the emulation root and fails, which is what happens on a real system with no
    // syslogd and what libSystem is written to tolerate.
    //
    // Every other address family is refused deliberately rather than for want of implementation. This
    // is an analysis sandbox: handing a sample a real AF_INET socket would let it reach the network
    // from inside what is supposed to be a container, and that has to be an explicit decision by
    // whoever runs it, not a side effect of a syscall being convenient to add.
    void sys_socket(const macos_syscall_context& c)
    {
        constexpr int32_t MACOS_AF_UNIX = 1;

        const auto domain = static_cast<int32_t>(get_macos_syscall_argument(c, 0));
        const auto type = static_cast<int32_t>(get_macos_syscall_argument(c, 1));
        const auto protocol = static_cast<int32_t>(get_macos_syscall_argument(c, 2));

        if (domain != MACOS_AF_UNIX)
        {
            c.emu_ref.log.warn("refusing socket(domain=%d, type=%d): only local sockets exist in the sandbox\n", domain, type);
            write_macos_syscall_error(c, MACOS_EAFNOSUPPORT);
            return;
        }

        guest_fd entry{};
        entry.type = fd_type::socket;
        entry.socket_state = std::make_shared<guest_socket_state>();
        entry.socket_state->domain = domain;
        entry.socket_state->type = type;
        entry.socket_state->protocol = protocol;

        const auto fd = c.proc.fds.allocate(std::move(entry));
        if (fd < 0)
        {
            write_macos_syscall_error(c, MACOS_EMFILE);
            return;
        }

        write_macos_syscall_result(c, fd);
    }

    // The daemons a local socket would reach -- syslogd, notifyd -- do not exist in the emulation root,
    // so a connect to one fails exactly as it does on a system without them, and libSystem degrades to
    // not logging. Two outcomes, and the difference matters:
    //
    //   the path is not there            -> ENOENT, which is simply true
    //   the path *is* there              -> refused and logged, because that socket belongs to the
    //                                       host, and letting a sample speak to a real daemon through
    //                                       the emulation root is an escape, not an emulation
    void sys_connect(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto address = get_macos_syscall_argument(c, 1);
        const auto length = get_macos_syscall_argument(c, 2);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (entry->type != fd_type::socket || !entry->socket_state)
        {
            write_macos_syscall_error(c, MACOS_ENOTSOCK);
            return;
        }

        // sockaddr_un: uint8 len, uint8 family, then the path.
        constexpr size_t SOCKADDR_UN_PATH_OFFSET = 2;
        if (length <= SOCKADDR_UN_PATH_OFFSET || length > MACOS_PATH_MAX)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        std::vector<char> raw(static_cast<size_t>(length), 0);
        if (!c.emu_ref.memory.try_read_memory(address, raw.data(), raw.size()))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        const std::string guest_path{raw.data() + SOCKADDR_UN_PATH_OFFSET,
                                     std::min(raw.size() - SOCKADDR_UN_PATH_OFFSET, std::strlen(raw.data() + SOCKADDR_UN_PATH_OFFSET))};

        std::error_code error{};
        const auto host_path = c.emu_ref.file_sys.translate(guest_path);

        if (std::filesystem::exists(host_path, error) && !error)
        {
            c.emu_ref.log.warn("refusing to connect to \"%s\": that socket belongs to the host\n", guest_path.c_str());
            write_macos_syscall_error(c, MACOS_ECONNREFUSED);
            return;
        }

        c.emu_ref.log.print(color::gray, "connect(\"%s\") has no daemon to reach\n", guest_path.c_str());
        write_macos_syscall_error(c, MACOS_ENOENT);
    }

    void sys_read(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto address = get_macos_syscall_argument(c, 1);
        const auto length = get_macos_syscall_argument(c, 2);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (length > MACOS_IO_MAX_TRANSFER)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        int64_t error = 0;
        const auto received = transfer_from_fd(c, *entry, address, length, error);
        if (error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        write_macos_syscall_result(c, received);
    }

    // dyld reads the shared cache header and its subcache table through pread rather than seeking,
    // because it keeps one descriptor and reads many offsets from it. POSIX requires the file offset to
    // be left alone, which is the whole reason it uses this call.
    void sys_pread(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto address = get_macos_syscall_argument(c, 1);
        const auto length = get_macos_syscall_argument(c, 2);
        const auto offset = static_cast<int64_t>(get_macos_syscall_argument(c, 3));

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (length > MACOS_IO_MAX_TRANSFER)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        if (offset < 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        if (entry->type == fd_type::directory || entry->type == fd_type::pipe_read || entry->type == fd_type::pipe_write)
        {
            write_macos_syscall_error(c, MACOS_ESPIPE);
            return;
        }

        if (entry->type == fd_type::memory_file)
        {
            if (!entry->memory_file)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            const auto saved = entry->memory_file->offset;
            entry->memory_file->offset =
                static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(offset), entry->memory_file->content.size()));

            int64_t error = 0;
            const auto received = transfer_from_fd(c, *entry, address, length, error);
            entry->memory_file->offset = saved;

            if (error != 0)
            {
                write_macos_syscall_error(c, error);
                return;
            }

            write_macos_syscall_result(c, received);
            return;
        }

        if (entry->handle == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        const auto saved = ftello(entry->handle);
        if (saved < 0 || fseeko(entry->handle, static_cast<off_t>(offset), SEEK_SET) != 0)
        {
            write_macos_syscall_error(c, MACOS_ESPIPE);
            return;
        }

        int64_t error = 0;
        const auto received = transfer_from_fd(c, *entry, address, length, error);

        if (fseeko(entry->handle, saved, SEEK_SET) != 0)
        {
            write_macos_syscall_error(c, MACOS_EIO);
            return;
        }

        if (error != 0)
        {
            write_macos_syscall_error(c, error);
            return;
        }

        write_macos_syscall_result(c, received);
    }

    void sys_writev(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto address = get_macos_syscall_argument(c, 1);
        const auto iovcnt = static_cast<int64_t>(static_cast<int32_t>(get_macos_syscall_argument(c, 2)));

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_WRITE, "writev"))
        {
            return;
        }

        int64_t error = 0;
        std::vector<macos_iovec> vectors{};
        if (!read_iovec_array(c, address, iovcnt, vectors, error))
        {
            write_macos_syscall_error(c, error);
            return;
        }

        const auto fd_error = writable_fd_error(*entry);
        if (fd_error != 0)
        {
            write_macos_syscall_error(c, fd_error);
            return;
        }

        int64_t total = 0;
        for (const auto& vector : vectors)
        {
            const auto written = transfer_to_fd(c, *entry, vector.iov_base, vector.iov_len, error);
            total += written;

            if (error != 0)
            {
                if (total == 0)
                {
                    write_macos_syscall_error(c, error);
                    return;
                }

                break;
            }

            if (static_cast<uint64_t>(written) < vector.iov_len)
            {
                break;
            }
        }

        write_macos_syscall_result(c, total);
    }

    void sys_readv(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto address = get_macos_syscall_argument(c, 1);
        const auto iovcnt = static_cast<int64_t>(static_cast<int32_t>(get_macos_syscall_argument(c, 2)));

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        int64_t error = 0;
        std::vector<macos_iovec> vectors{};
        if (!read_iovec_array(c, address, iovcnt, vectors, error))
        {
            write_macos_syscall_error(c, error);
            return;
        }

        int64_t total = 0;
        for (const auto& vector : vectors)
        {
            const auto received = transfer_from_fd(c, *entry, vector.iov_base, vector.iov_len, error);
            total += received;

            if (error != 0)
            {
                if (total == 0)
                {
                    write_macos_syscall_error(c, error);
                    return;
                }

                break;
            }

            if (static_cast<uint64_t>(received) < vector.iov_len)
            {
                break;
            }
        }

        write_macos_syscall_result(c, total);
    }

    void sys_close(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_CLOSE, "close"))
        {
            return;
        }

        close_descriptor(c, fd);
    }

    // The rest of what libsqlite3 does to a database file once it is open, read off that library's own
    // import table rather than discovered one stalled run at a time.
    void sys_fsync(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (entry->handle == nullptr || guest_fd_detail::is_stdio_path(entry->host_path))
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        if (fflush(entry->handle) != 0)
        {
            write_macos_syscall_error(c, map_host_errno_to_macos(errno));
            return;
        }

#if defined(_WIN32)
        if (_commit(_fileno(entry->handle)) != 0)
#else
        if (::fsync(fileno(entry->handle)) != 0)
#endif
        {
            write_macos_syscall_error(c, map_host_errno_to_macos(errno));
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // The emulation root is the analyst's own directory tree. A guest that hands its store to another
    // uid, or takes its own read permission away, would be changing the host's files in a way the run
    // cannot undo and the emulator itself may not survive -- the same reason mkdir drops the mode it is
    // given. Both calls are accepted so the caller's error handling is not exercised over nothing.
    void sys_fchown(const macos_syscall_context& c)
    {
        accept_descriptor_metadata_change(c);
    }

    void sys_fchmod(const macos_syscall_context& c)
    {
        accept_descriptor_metadata_change(c);
    }

    void sys_ftruncate(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto length = static_cast<int64_t>(get_macos_syscall_argument(c, 1));

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (length < 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        if (entry->type != fd_type::file || entry->handle == nullptr || entry->read_only_mapping)
        {
            write_macos_syscall_error(c, entry->read_only_mapping ? MACOS_EACCES : MACOS_EINVAL);
            return;
        }

        // The stream is unbuffered, but a flush before the resize is what keeps a write that is still in
        // flight from landing past the new end of the file.
        if (fflush(entry->handle) != 0)
        {
            write_macos_syscall_error(c, map_host_errno_to_macos(errno));
            return;
        }

        std::error_code error{};
        std::filesystem::resize_file(std::filesystem::path{entry->host_path}, static_cast<uintmax_t>(length), error);
        if (error)
        {
            write_macos_syscall_error(c, map_host_errno_to_macos(error.default_error_condition().value()));
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // libsqlite3 touches the proxy conch to say a lock is still live, and passes a null tptr for "now",
    // which is the only form sogen has seen. A supplied pair carries an access time as well as a
    // modification time; only the modification time survives here, because that is the one every host
    // filesystem this runs on agrees to store.
    void sys_futimes(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto times = get_macos_syscall_argument(c, 1);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (entry->host_path.empty() || guest_fd_detail::is_stdio_path(entry->host_path))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        auto modified = std::filesystem::file_time_type::clock::now();

        if (times != 0)
        {
            std::array<macos_timeval, 2> requested{};
            if (!c.emu_ref.memory.try_read_memory(times, requested.data(), sizeof(requested)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            const auto seconds = std::chrono::seconds{requested[1].tv_sec};
            const auto micros = std::chrono::microseconds{requested[1].tv_usec};
            modified =
                std::filesystem::file_time_type{std::chrono::duration_cast<std::filesystem::file_time_type::duration>(seconds + micros)};
        }

        std::error_code error{};
        std::filesystem::last_write_time(std::filesystem::path{entry->host_path}, modified, error);
        if (error)
        {
            write_macos_syscall_error(c, map_host_errno_to_macos(error.default_error_condition().value()));
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    void sys_lseek(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto offset = static_cast<int64_t>(get_macos_syscall_argument(c, 1));
        const auto whence = static_cast<int32_t>(get_macos_syscall_argument(c, 2));

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (whence != MACOS_SEEK_SET && whence != MACOS_SEEK_CUR && whence != MACOS_SEEK_END)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        if (entry->type == fd_type::memory_file)
        {
            if (!entry->memory_file)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            auto& file = *entry->memory_file;
            const auto base = memory_file_seek_base(whence, file);

            if (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            const auto position = base + offset;
            if (position < 0)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            // guest_fd_table's snapshot rejects a memory-file offset past its content, so a seek beyond
            // the end settles at the end instead of being stored sparsely.
            file.offset = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(position), file.content.size()));
            write_macos_syscall_result(c, static_cast<int64_t>(file.offset));
            return;
        }

        // The stdio streams are the host's own; seeking them would move a descriptor the emulator does
        // not own, and Darwin reports a terminal or pipe as unseekable anyway.
        if (guest_fd_detail::is_stdio_path(entry->host_path))
        {
            write_macos_syscall_error(c, MACOS_ESPIPE);
            return;
        }

        if (entry->handle == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        const auto origin = host_seek_origin(whence);
        if (seek_host_file(entry->handle, offset, origin) != 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto position = tell_host_file(entry->handle);
        if (position < 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        write_macos_syscall_result(c, position);
    }

    void sys_fcntl(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);
        const auto command = static_cast<int32_t>(get_macos_syscall_argument(c, 1));
        const auto argument = get_macos_syscall_argument(c, 2);

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        switch (command)
        {
        case MACOS_F_GETFD:
            write_macos_syscall_result(c, entry->close_on_exec ? MACOS_FD_CLOEXEC : 0);
            return;

        case MACOS_F_SETFD:
            entry->close_on_exec = (argument & static_cast<uint64_t>(MACOS_FD_CLOEXEC)) != 0;
            write_macos_syscall_result(c, 0);
            return;

        case MACOS_F_GETFL:
            write_macos_syscall_result(c, entry->flags);
            return;

        case MACOS_F_SETFL:
            entry->flags = static_cast<int32_t>(argument);
            write_macos_syscall_result(c, 0);
            return;

        case MACOS_F_CHECK_LV:
            // Library validation: dyld asks whether the signature it just registered is one this
            // process is allowed to load. A hardened process may only load Apple-signed or
            // same-team-signed code; sogen models a kernel that enforces no code signing at all, for
            // the same reason it verifies none -- an unsigned or tampered binary has to stay
            // analysable. Refusing this is not neutral: dyld reports "code signature not valid for use
            // in process" and the objc runtime loses its trampolines, which is fatal to any Swift or
            // AppKit process.
            write_macos_syscall_result(c, 0);
            return;

        case MACOS_F_ADDFILESIGS_RETURN: {
            // dyld registers the shared cache's code signature this way and abandons the cache when the
            // call fails, so this is what decides whether a guest gets a cache at all. sogen verifies no
            // signature on purpose -- it models a permissive kernel so that unsigned and tampered
            // binaries are analysable -- but the caller still needs the end offset it asked for, which
            // it uses to bound its own reads.
            std::error_code size_error{};
            const auto file_size = std::filesystem::file_size(std::filesystem::path{entry->host_path}, size_error);
            if (size_error)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }

            const auto signed_end = static_cast<int64_t>(file_size);
            if (!c.emu_ref.memory.try_write_memory(argument, &signed_end, sizeof(signed_end)))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        case MACOS_F_GETPATH:
        case MACOS_F_GETPATH_NOFIRMLINK: {
            // dyld asks the shared cache descriptor for its own path and abandons the cache when the
            // answer does not come back, so this is the difference between running with a cache and
            // running without one. The guest path is the right answer: the guest has never seen the
            // host one, and there are no firmlinks in a synthetic root for the second command to strip.
            const auto& path = entry->guest_path;
            if (path.size() + 1 > MACOS_PATH_MAX)
            {
                write_macos_syscall_error(c, MACOS_ENAMETOOLONG);
                return;
            }

            // vn_getpath sets the copyout length from the path it built, so xnu writes the string and
            // its terminator and stops there. Writing the whole MAXPATHLEN scratch buffer would reach
            // past what the caller asked the kernel to touch.
            if (!c.emu_ref.memory.try_write_memory(argument, path.c_str(), path.size() + 1))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return;
            }

            write_macos_syscall_result(c, 0);
            return;
        }

        case MACOS_F_GETLK:
        case MACOS_F_SETLK:
        case MACOS_F_SETLKW:
        case MACOS_F_OFD_GETLK:
        case MACOS_F_OFD_SETLK:
        case MACOS_F_OFD_SETLKW:
            file_lock_command(c, fd, *entry, command, argument);
            return;

        case MACOS_F_DUPFD:
        case MACOS_F_DUPFD_CLOEXEC: {
            if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_DUP, "fcntl(F_DUPFD)"))
            {
                return;
            }

            const auto minimum = static_cast<int64_t>(static_cast<int32_t>(argument));
            if (minimum < 0 || minimum >= MACOS_MAX_OPEN_DESCRIPTORS)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return;
            }

            int new_fd = -1;
            const auto duplicate_error = duplicate_descriptor(c, fd, static_cast<int>(minimum), new_fd);
            if (duplicate_error != 0)
            {
                write_macos_syscall_error(c, duplicate_error);
                return;
            }

            if (command == MACOS_F_DUPFD_CLOEXEC)
            {
                c.proc.fds.set_close_on_exec(new_fd, true);
            }

            write_macos_syscall_result(c, new_fd);
            return;
        }

        default:
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }
    }

    // dyld's ioctl calls are terminal queries it tolerates failing, and inventing request encodings the
    // emulator has never observed would be fabrication.
    void sys_ioctl(const macos_syscall_context& c)
    {
        write_macos_syscall_error(c, MACOS_ENOTTY);
    }

    void sys_dup(const macos_syscall_context& c)
    {
        const auto fd = read_syscall_fd(c, 0);

        if (c.proc.fds.get(fd) == nullptr)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (!fd_guard_permits_plain_call(c, fd, MACOS_GUARD_DUP, "dup"))
        {
            return;
        }

        int new_fd = -1;
        const auto duplicate_error = duplicate_descriptor(c, fd, 0, new_fd);
        if (duplicate_error != 0)
        {
            write_macos_syscall_error(c, duplicate_error);
            return;
        }

        write_macos_syscall_result(c, new_fd);
    }

    void sys_dup2(const macos_syscall_context& c)
    {
        const auto old_fd = read_syscall_fd(c, 0);
        const auto new_fd = read_syscall_fd(c, 1);

        if (new_fd < 0 || new_fd >= MACOS_MAX_OPEN_DESCRIPTORS)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        // Two guards can be in play: dup2 duplicates one descriptor and closes whatever occupied the
        // target, so it is both a dup of the source and a close of the destination.
        if (!fd_guard_permits_plain_call(c, old_fd, MACOS_GUARD_DUP, "dup2") ||
            (old_fd != new_fd && !fd_guard_permits_plain_call(c, new_fd, MACOS_GUARD_CLOSE, "dup2")))
        {
            return;
        }

        const auto result = c.proc.fds.dup2_fd(old_fd, new_fd);
        if (result < 0)
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        c.proc.forget_descriptor_state(new_fd);
        write_macos_syscall_result(c, result);
    }

}
