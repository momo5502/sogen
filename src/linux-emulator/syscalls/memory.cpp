#include "../std_include.hpp"
#include "../linux_emulator.hpp"
#include "../linux_syscall_dispatcher.hpp"

#include <address_utils.hpp>

using namespace linux_errno;

namespace
{
    constexpr int PROT_READ = 1;
    constexpr int PROT_WRITE = 2;
    constexpr int PROT_EXEC = 4;

    constexpr int MAP_FIXED = 0x10;
    constexpr int MAP_FIXED_NOREPLACE = 0x100000;
    constexpr int MAP_ANONYMOUS = 0x20;

    memory_permission prot_to_permission(const int prot)
    {
        memory_permission perm = memory_permission::none;

        if (prot & PROT_READ)
        {
            perm = perm | memory_permission::read;
        }
        if (prot & PROT_WRITE)
        {
            perm = perm | memory_permission::write;
        }
        if (prot & PROT_EXEC)
        {
            perm = perm | memory_permission::exec;
        }

        return perm;
    }
}

void sys_mmap(const linux_syscall_context& c)
{
    const auto addr = get_linux_syscall_argument(c.emu, 0);
    const auto length = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
    const auto prot = static_cast<int>(get_linux_syscall_argument(c.emu, 2));
    const auto flags = static_cast<int>(get_linux_syscall_argument(c.emu, 3));
    const auto fd = static_cast<int>(get_linux_syscall_argument(c.emu, 4));
    const auto offset = get_linux_syscall_argument(c.emu, 5);

    if (length == 0)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto aligned_length = page_align_up(length);
    const auto perms = prot_to_permission(prot);
    const bool is_anonymous = (flags & MAP_ANONYMOUS) != 0;

    auto& memory = c.emu_ref.memory;
    uint64_t mapped_addr = 0;

    if (flags & MAP_FIXED_NOREPLACE)
    {
        if (addr == 0 || page_align_down(addr) != addr)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        const auto aligned_addr = addr;

        // Do not replace existing mappings; fail with EEXIST if occupied.
        if (memory.overlaps_mapped_region(aligned_addr, aligned_length))
        {
            write_linux_syscall_result(c, -LINUX_EEXIST);
            return;
        }

        if (!memory.allocate_memory(aligned_addr, aligned_length, memory_permission::read_write))
        {
            write_linux_syscall_result(c, -LINUX_ENOMEM);
            return;
        }

        mapped_addr = aligned_addr;
    }
    else if (flags & MAP_FIXED)
    {
        if (addr == 0)
        {
            write_linux_syscall_result(c, -LINUX_EINVAL);
            return;
        }

        const auto aligned_addr = page_align_down(addr);

        // Release any existing mapping in the range first
        memory.release_memory(aligned_addr, aligned_length);

        // Allocate as read-write initially so we can write file data
        if (!memory.allocate_memory(aligned_addr, aligned_length, memory_permission::read_write))
        {
            write_linux_syscall_result(c, -LINUX_ENOMEM);
            return;
        }

        mapped_addr = aligned_addr;
    }
    else
    {
        // Find a free region — allocate as read-write initially so we can write file data
        mapped_addr = memory.allocate_memory(aligned_length, memory_permission::read_write, addr ? addr : 0);
        if (!mapped_addr)
        {
            write_linux_syscall_result(c, -LINUX_ENOMEM);
            return;
        }
    }

    // If file-backed, read data from the fd into the mapped region
    if (!is_anonymous)
    {
        auto* fd_entry = c.proc.fds.get(fd);
        if (!fd_entry || !fd_entry->handle)
        {
            // Can't read from this fd — unmap and fail
            memory.release_memory(mapped_addr, aligned_length);
            write_linux_syscall_result(c, -LINUX_EBADF);
            return;
        }

        // Save current file position, seek to offset, read data, restore position
        const auto saved_pos = ftell(fd_entry->handle);
        fseek(fd_entry->handle, static_cast<long>(offset), SEEK_SET);

        // Read min(length, file_remaining) bytes — don't read beyond the file
        const auto bytes_to_read = length; // length (not aligned_length) — the rest is zero-fill
        std::vector<uint8_t> buffer(bytes_to_read);
        const auto bytes_read = fread(buffer.data(), 1, bytes_to_read, fd_entry->handle);

        fseek(fd_entry->handle, saved_pos, SEEK_SET);

        if (bytes_read > 0)
        {
            c.emu.write_memory(mapped_addr, buffer.data(), bytes_read);
        }
        // Remaining bytes (up to aligned_length) are already zero from fresh allocation
    }

    // Now set the actual requested permissions
    if (perms != memory_permission::read_write)
    {
        memory.protect_memory(mapped_addr, aligned_length, perms);
    }

    write_linux_syscall_result(c, static_cast<int64_t>(mapped_addr));
}

void sys_mprotect(const linux_syscall_context& c)
{
    const auto addr = get_linux_syscall_argument(c.emu, 0);
    const auto length = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));
    const auto prot = static_cast<int>(get_linux_syscall_argument(c.emu, 2));

    if (length == 0 || page_align_down(addr) != addr)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto aligned_length = page_align_up(length);

    const auto perms = prot_to_permission(prot);

    if (c.emu_ref.memory.protect_memory(addr, aligned_length, perms))
    {
        write_linux_syscall_result(c, 0);
    }
    else
    {
        write_linux_syscall_result(c, -LINUX_ENOMEM);
    }
}

void sys_munmap(const linux_syscall_context& c)
{
    const auto addr = get_linux_syscall_argument(c.emu, 0);
    const auto length = static_cast<size_t>(get_linux_syscall_argument(c.emu, 1));

    if (length == 0 || page_align_down(addr) != addr)
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
        return;
    }

    const auto aligned_length = page_align_up(length);

    if (c.emu_ref.memory.release_memory(addr, aligned_length))
    {
        write_linux_syscall_result(c, 0);
    }
    else
    {
        write_linux_syscall_result(c, -LINUX_EINVAL);
    }
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

    const auto aligned_old_size = page_align_up(old_size);
    const auto aligned_new_size = page_align_up(new_size);

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
        const auto clear_size = aligned_end - aligned_addr;

        std::vector<uint8_t> zeros(clear_size, 0);
        c.emu.try_write_memory(aligned_addr, zeros.data(), clear_size);
    }

    // All other advice values are no-ops
    write_linux_syscall_result(c, 0);
}
