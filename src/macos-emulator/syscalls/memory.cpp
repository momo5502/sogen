#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_syscall_utils.hpp"

#include <address_utils.hpp>

#include <cinttypes>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;
    using namespace macos_mmap;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        // sys/mman.h. Its siblings live in macos_platform.hpp's macos_mmap namespace and this belongs
        // beside them; it is here only because that header is owned elsewhere in this change.

        memory_permission translate_protection(const int32_t protection)
        {
            return macos_prot_to_permission(protection);
        }

        // MAP_FIXED replaces whatever is already mapped over the range instead of failing on it. dyld
        // relies on exactly that: it reserves an image's whole span with vm_allocate and then maps each
        // segment into that span, so a mapping that refuses the overlap makes every dylib outside the
        // shared cache unloadable -- libobjc-trampolines.dylib among them, which is fatal to any Swift
        // or AppKit process.
        void clear_fixed_range(const macos_syscall_context& c, const uint64_t base, const size_t size)
        {
            if (macos_memory_manager::is_reserved_range(base, size))
            {
                return;
            }

            c.emu_ref.memory.release_memory(base, size);
        }

        void map_anonymous(const macos_syscall_context& c, const uint64_t address, const size_t size, const int32_t flags,
                           const memory_permission permissions)
        {
            if ((flags & MACOS_MAP_FIXED) != 0)
            {
                const auto base = page_align_down(address, MACOS_PAGE_SIZE);
                clear_fixed_range(c, base, size);

                if (!c.emu_ref.memory.allocate_memory(base, size, permissions))
                {
                    write_macos_syscall_error(c, MACOS_ENOMEM);
                    return;
                }

                c.emu_ref.callbacks.on_memory_allocate(base, size, permissions, true);
                write_macos_syscall_result(c, static_cast<int64_t>(base));
                return;
            }

            const auto base = c.emu_ref.memory.allocate_memory(size, permissions);
            if (base == 0)
            {
                write_macos_syscall_error(c, MACOS_ENOMEM);
                return;
            }

            c.emu_ref.callbacks.on_memory_allocate(base, size, permissions, true);
            write_macos_syscall_result(c, static_cast<int64_t>(base));
        }
    }

    void sys_mmap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);
        const auto protection = static_cast<int32_t>(get_macos_syscall_argument(c, 2));
        const auto flags = static_cast<int32_t>(get_macos_syscall_argument(c, 3));
        const auto fd = static_cast<int>(static_cast<int32_t>(get_macos_syscall_argument(c, 4)));
        const auto file_offset = get_macos_syscall_argument(c, 5);

        if (length == 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        // Redundant with find_free_allocation_base and is_reserved_range, which refuse every length this
        // rejects, and deliberately kept: it states the bound where length is still the raw guest value,
        // and page_align_up below would otherwise be the first thing to touch it. No test can tell the
        // two layers apart, because an overflowing page_align_up can only produce 0, which
        // align_to_pages already refuses.
        if (length > MACOS_MAX_MMAP_END_EXCL)
        {
            write_macos_syscall_error(c, MACOS_ENOMEM);
            return;
        }

        const auto size = static_cast<size_t>(page_align_up(length, MACOS_PAGE_SIZE));
        const auto permissions = translate_protection(protection);

        if ((flags & MACOS_MAP_ANON) != 0 || fd < 0)
        {
            map_anonymous(c, address, size, flags, permissions);
            return;
        }

        auto* entry = c.proc.fds.get(fd);
        if (entry == nullptr || entry->type != fd_type::file || entry->host_path.empty() ||
            guest_fd_detail::is_stdio_path(entry->host_path))
        {
            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        const auto fixed = (flags & MACOS_MAP_FIXED) != 0;
        const auto base = fixed ? page_align_down(address, MACOS_PAGE_SIZE) : c.emu_ref.memory.find_free_allocation_base(size);

        if (fixed)
        {
            clear_fixed_range(c, base, size);
        }

        const std::filesystem::path host_path{entry->host_path};

        // Order matters: the copying path succeeds for everything, so trying it first would mean the
        // gigabyte-scale shared cache is never zero-copied. map_host_file_range refuses a
        // host-unalignable offset and a non-regular file, which is exactly when copying is the only
        // option left.
        const auto mapped = base != 0 && (c.emu_ref.memory.map_host_file_range(base, size, host_path, file_offset, permissions) ||
                                          c.emu_ref.memory.map_file(base, size, permissions, host_path, file_offset));

        if (!mapped)
        {
            write_macos_syscall_error(c, MACOS_ENOMEM);
            return;
        }

        c.emu_ref.callbacks.on_memory_allocate(base, size, permissions, true);
        write_macos_syscall_result(c, static_cast<int64_t>(base));
    }

    void sys_munmap(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);

        if (length == 0 || !c.emu_ref.memory.release_memory(address, static_cast<size_t>(length)))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        c.emu_ref.callbacks.on_memory_release(address, length);
        write_macos_syscall_result(c, 0);
    }

    void sys_mprotect(const macos_syscall_context& c)
    {
        const auto address = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);
        const auto protection = static_cast<int32_t>(get_macos_syscall_argument(c, 2));
        const auto permissions = translate_protection(protection);

        if (length == 0 || !c.emu_ref.memory.protect_memory(address, static_cast<size_t>(length), permissions))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        c.emu_ref.callbacks.on_memory_protect(address, length, permissions);
        write_macos_syscall_result(c, 0);
    }

    // Every Darwin MADV_* value is advisory except MADV_ZERO, which the caller is entitled to treat as
    // having cleared the range -- madvise(2) says a kernel that cannot do it has to answer ENOTSUP so
    // the caller zeroes the range itself. libmalloc's calloc path uses it in place of a memset for
    // page-sized blocks, so reporting success without zeroing hands the guest a dirty buffer it will
    // never clear: measured as a CFBasicHash bucket array full of stale pointers, which CoreFoundation
    // then released as if they were the values it had stored.
    void sys_madvise(const macos_syscall_context& c)
    {
        const auto behavior = static_cast<int32_t>(get_macos_syscall_argument(c, 2));
        if (behavior != macos_mmap::MACOS_MADV_ZERO)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        const auto address = get_macos_syscall_argument(c, 0);
        const auto length = get_macos_syscall_argument(c, 1);

        if (length == 0)
        {
            write_macos_syscall_result(c, 0);
            return;
        }

        if (!c.emu_ref.memory.zero_memory(address, static_cast<size_t>(length)))
        {
            c.emu_ref.log.warn("MADV_ZERO over 0x%" PRIx64 " + 0x%" PRIx64 " is not a mapped writable range\n", address, length);
            write_macos_syscall_error(c, MACOS_ENOTSUP);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

    // Failing here is the mechanism that forces dyld onto DYLD_SHARED_REGION=private, per
 //.1(C). Reporting a shared region
    // would send it down the kernel-assisted path, which sogen cannot serve.
    // dyld calls this to ask whether a shared region is already mapped, and treats the failure as
    // "this process has no cache". Before shared_region_map_and_slide_2_np installs one there is
    // genuinely nothing to report; afterwards the base address is what dyld builds every cache-relative
    // address from.
    void sys_shared_region_check_np(const macos_syscall_context& c)
    {
        if (c.proc.shared_region_base == 0)
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto out = get_macos_syscall_argument(c, 0);
        if (out != 0 && !c.emu_ref.memory.try_write_memory(out, &c.proc.shared_region_base, sizeof(c.proc.shared_region_base)))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, 0);
    }

}
