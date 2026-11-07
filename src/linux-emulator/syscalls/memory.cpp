#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <address_utils.hpp>
#include <posix_memory_syscalls.hpp>

namespace sogen
{

    using namespace linux_errno; // NOLINT(google-build-using-namespace)

    namespace
    {
        constexpr int MAP_FIXED = 0x10;
        constexpr int MAP_FIXED_NOREPLACE = 0x100000;
        constexpr int MAP_ANONYMOUS = 0x20;

        posix_map_placement map_placement(const int flags)
        {
            if (flags & MAP_FIXED_NOREPLACE)
            {
                return posix_map_placement::fixed_noreplace;
            }

            return (flags & MAP_FIXED) ? posix_map_placement::fixed : posix_map_placement::hint;
        }
    }

    void sys_mmap(const linux_syscall_context& c)
    {
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));
        const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 4));
        const posix_map_request request{
            .address = get_linux_syscall_argument(c.emu, 0),
            .length = get_linux_syscall_argument(c.emu, 1),
            .protection = get_linux_syscall_argument(c.emu, 2),
            .placement = map_placement(flags),
            .fd = (flags & MAP_ANONYMOUS) ? std::nullopt : std::optional{fd},
            .offset = get_linux_syscall_argument(c.emu, 5),
        };

        const auto result = posix_mmap(c.emu_ref.memory, c.proc.fds, request, posix_errno::enomem);
        const auto* address = std::get_if<uint64_t>(&result);
        const auto* file = request.fd ? c.proc.fds.get(*request.fd) : nullptr;
        if (address && file && !file->host_path.empty() && (request.protection & POSIX_PROT_EXEC))
        {
            c.emu_ref.mod_manager.record_runtime_module_mapping(file->host_path, *address, request.offset);
        }

        write_linux_syscall_result(c, result);
    }

    void sys_mprotect(const linux_syscall_context& c)
    {
        const auto addr = get_linux_syscall_argument(c.emu, 0);
        const auto length = get_linux_syscall_argument(c.emu, 1);
        const auto prot = get_linux_syscall_argument(c.emu, 2);
        write_linux_syscall_result(c, posix_mprotect(c.emu_ref.memory, addr, length, prot, posix_errno::enomem));
    }

    void sys_munmap(const linux_syscall_context& c)
    {
        const auto addr = get_linux_syscall_argument(c.emu, 0);
        const auto length = get_linux_syscall_argument(c.emu, 1);
        write_linux_syscall_result(c, posix_munmap(c.emu_ref.memory, addr, length));
    }

    void sys_brk(const linux_syscall_context& c)
    {
        const auto new_brk = get_linux_syscall_argument(c.emu, 0);
        auto& proc = c.proc;

        // brk(0) returns the current break
        if (new_brk == 0)
        {
            write_linux_syscall_result(c, static_cast<int64_t>(proc.brk_current));
            return;
        }

        const auto aligned_new = page_align_up(new_brk);
        const auto aligned_current = page_align_up(proc.brk_current);

        if (aligned_new > aligned_current)
        {
            // Need to allocate more memory
            const auto alloc_size = static_cast<size_t>(aligned_new - aligned_current);

            if (!c.emu_ref.memory.allocate_memory(aligned_current, alloc_size, memory_permission::read_write))
            {
                // Failed to allocate — return current brk (failure)
                write_linux_syscall_result(c, static_cast<int64_t>(proc.brk_current));
                return;
            }
        }
        else if (aligned_new < aligned_current)
        {
            // Shrink — release pages
            const auto release_size = static_cast<size_t>(aligned_current - aligned_new);
            c.emu_ref.memory.release_memory(aligned_new, release_size);
        }

        proc.brk_current = new_brk;
        write_linux_syscall_result(c, static_cast<int64_t>(new_brk));
    }

    // --- Phase 4b: Additional memory syscalls ---

    void sys_mremap(const linux_syscall_context& c)
    {
        const auto old_address = get_linux_syscall_argument(c.emu, 0);
        const auto old_size = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
        const auto new_size = static_cast<size_t>(get_linux_syscall_argument(c.emu, 2));
        const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));
        // new_address is arg 4 (only used with MREMAP_FIXED)

        constexpr int MREMAP_MAYMOVE = 1;

        if (new_size == 0)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        const auto aligned_old_size = static_cast<size_t>(page_align_up(old_size));
        const auto aligned_new_size = static_cast<size_t>(page_align_up(new_size));

        if (aligned_new_size <= aligned_old_size)
        {
            // Shrinking: unmap the tail
            if (aligned_new_size < aligned_old_size)
            {
                c.emu_ref.memory.release_memory(old_address + aligned_new_size, aligned_old_size - aligned_new_size);
            }
            write_linux_syscall_result(c, static_cast<int64_t>(old_address));
            return;
        }

        // Growing: try to expand in place first
        const auto grow_amount = aligned_new_size - aligned_old_size;
        const auto expand_addr = old_address + aligned_old_size;

        if (!c.emu_ref.memory.overlaps_mapped_region(expand_addr, grow_amount))
        {
            if (c.emu_ref.memory.allocate_memory(expand_addr, grow_amount, memory_permission::read_write))
            {
                write_linux_syscall_result(c, static_cast<int64_t>(old_address));
                return;
            }
        }

        if (!(flags & MREMAP_MAYMOVE))
        {
            write_linux_syscall_result(c, -LINUX_ENOMEM);
            return;
        }

        // Move: allocate new region, copy data, release old
        const auto new_base = c.emu_ref.memory.allocate_memory(aligned_new_size, memory_permission::read_write);
        if (!new_base)
        {
            write_linux_syscall_result(c, -LINUX_ENOMEM);
            return;
        }

        // Copy old data to new location
        std::vector<uint8_t> buffer(aligned_old_size);
        c.emu.read_memory(old_address, buffer.data(), aligned_old_size);
        c.emu.write_memory(new_base, buffer.data(), aligned_old_size);

        // Release old mapping
        c.emu_ref.memory.release_memory(old_address, aligned_old_size);

        write_linux_syscall_result(c, static_cast<int64_t>(new_base));
    }

    void sys_madvise(const linux_syscall_context& c)
    {
        const auto addr = get_linux_syscall_argument(c.emu, 0);
        const auto length = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
        const auto advice = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

        constexpr int MADV_DONTNEED = 4;

        if (advice == MADV_DONTNEED)
        {
            // Zero out the pages (simulates the kernel discarding them)
            const auto aligned_addr = page_align_down(addr);
            const auto aligned_end = page_align_up(addr + length);
            const auto clear_size = static_cast<size_t>(aligned_end - aligned_addr);

            std::vector<uint8_t> zeros(clear_size, 0);
            c.emu.try_write_memory(aligned_addr, zeros.data(), clear_size);
        }

        // All other advice values are no-ops
        write_linux_syscall_result(c, 0);
    }

} // namespace sogen
