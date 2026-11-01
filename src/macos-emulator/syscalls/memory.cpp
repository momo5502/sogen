#include "macos_syscalls.hpp"
#include "../macos_emulator.hpp"

#include "../macos_memory_protection.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace sogen
{
    namespace
    {
        constexpr size_t FILE_COPY_CHUNK_SIZE = 0x10000;

        void copy_file_to_memory(posix_memory_manager& memory, macos_file& file, const uint64_t offset, const uint64_t address,
                                 const size_t length)
        {
            std::vector<std::byte> chunk(std::min(length, FILE_COPY_CHUNK_SIZE));
            size_t copied = 0;
            while (copied < length)
            {
                const auto chunk_size = std::min(chunk.size(), length - copied);
                const auto read = file.read_at(offset + copied, std::span{chunk}.first(chunk_size));
                memory.write_memory(address + copied, chunk.data(), read);
                if (read < chunk_size)
                {
                    return;
                }

                copied += read;
            }
        }
    }

    void sys_mmap(const macos_syscall_context& c)
    {
        constexpr uint64_t map_fixed = 0x10;
        constexpr uint64_t map_anonymous = 0x1000;

        const auto requested_address = get_macos_syscall_argument(c.emu, 0);
        const auto length = static_cast<size_t>(page_align_up(get_macos_syscall_argument(c.emu, 1)));
        const auto protection = get_macos_syscall_argument(c.emu, 2);
        const auto flags = get_macos_syscall_argument(c.emu, 3);
        const auto fd = static_cast<int>(get_macos_syscall_argument(c.emu, 4));
        const auto offset = get_macos_syscall_argument(c.emu, 5);

        if (length == 0 || page_align_down(offset) != offset ||
            ((flags & map_fixed) && (requested_address == 0 || page_align_down(requested_address) != requested_address)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        auto* file = (flags & map_anonymous) ? nullptr : c.emu_ref.fds.get(fd);
        if (!(flags & map_anonymous) && file == nullptr)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EBADF);
            return;
        }

        auto& memory = c.emu_ref.memory;
        uint64_t address = 0;
        if (flags & map_fixed)
        {
            if (memory.replace_memory(requested_address, length, memory_permission::read_write))
            {
                address = requested_address;
            }
        }
        else
        {
            address = memory.allocate_memory(length, memory_permission::read_write, requested_address);
        }

        if (address == 0)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
            return;
        }

        if (file != nullptr)
        {
            copy_file_to_memory(memory, *file, offset, address, length);
        }

        memory.protect_memory(address, length, to_memory_permission(protection));
        write_macos_syscall_success(c, address);
    }

    void sys_munmap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 0);
        const auto length = static_cast<size_t>(get_macos_syscall_argument(c.emu, 1));

        if (length == 0 || page_align_down(address) != address)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        c.emu_ref.memory.release_memory(address, length);
        write_macos_syscall_success(c, 0);
    }

    void sys_mprotect(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c.emu, 0);
        const auto length = static_cast<size_t>(get_macos_syscall_argument(c.emu, 1));
        const auto protection = get_macos_syscall_argument(c.emu, 2);

        if (length == 0 || page_align_down(address) != address)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        if (!c.emu_ref.memory.protect_memory(address, static_cast<size_t>(page_align_up(length)), to_memory_permission(protection)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_ENOMEM);
            return;
        }

        write_macos_syscall_success(c, 0);
    }

    void sys_shared_region_check_np(const macos_syscall_context& c)
    {
        if (!c.emu_ref.shared_region)
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EINVAL);
            return;
        }

        const auto start_address_out = get_macos_syscall_argument(c.emu, 0);
        const auto base_address = c.emu_ref.shared_region->base_address();
        if (!c.emu.try_write_memory(start_address_out, &base_address, sizeof(base_address)))
        {
            write_macos_syscall_error(c, macos_errno::MACOS_EFAULT);
            return;
        }

        write_macos_syscall_success(c, 0);
    }
}
