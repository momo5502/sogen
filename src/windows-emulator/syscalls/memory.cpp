#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, const handle process_handle,
                                         const uint64_t base_address, const uint32_t info_class,
                                         const uint64_t memory_information, const uint64_t memory_information_length,
                                         const emulator_object<uint64_t> return_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == MemoryWorkingSetExInformation || info_class == MemoryImageExtensionInformation)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == MemoryBasicInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(EMU_MEMORY_BASIC_INFORMATION64));
            }

            if (memory_information_length < sizeof(EMU_MEMORY_BASIC_INFORMATION64))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            const emulator_object<EMU_MEMORY_BASIC_INFORMATION64> info{c.emu, memory_information};

            info.access([&](EMU_MEMORY_BASIC_INFORMATION64& image_info) {
                const auto region_info = c.win_emu.memory.get_region_info(base_address);

                assert(!region_info.is_committed || region_info.is_reserved);
                const auto state = region_info.is_reserved ? MEM_RESERVE : MEM_FREE;
                image_info.State = region_info.is_committed ? MEM_COMMIT : state;
                image_info.BaseAddress = region_info.start;
                image_info.AllocationBase = region_info.allocation_base;
                image_info.PartitionId = 0;
                image_info.RegionSize = static_cast<int64_t>(region_info.length);

                image_info.Protect = map_emulator_to_nt_protection(region_info.permissions);
                image_info.AllocationProtect = map_emulator_to_nt_protection(region_info.initial_permissions);
                image_info.Type = MEM_PRIVATE;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == MemoryImageInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(MEMORY_IMAGE_INFORMATION64));
            }

            if (memory_information_length != sizeof(MEMORY_IMAGE_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto* mod = base_address == 0 //
                                  ? c.win_emu.mod_manager.executable
                                  : c.win_emu.mod_manager.find_by_address(base_address);

            if (!mod)
            {
                c.win_emu.log.error("Bad address for memory image request: 0x%" PRIx64 "\n", base_address);
                return STATUS_INVALID_ADDRESS;
            }

            const emulator_object<MEMORY_IMAGE_INFORMATION64> info{c.emu, memory_information};

            info.access([&](MEMORY_IMAGE_INFORMATION64& image_info) {
                image_info.ImageBase = mod->image_base;
                image_info.SizeOfImage = static_cast<int64_t>(mod->size_of_image);
                image_info.ImageFlags = 0;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == MemoryRegionInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(MEMORY_REGION_INFORMATION64));
            }

            if (memory_information_length < sizeof(MEMORY_REGION_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto region_info = c.win_emu.memory.get_region_info(base_address);
            if (!region_info.is_reserved)
            {
                return STATUS_INVALID_ADDRESS;
            }

            const emulator_object<MEMORY_REGION_INFORMATION64> info{c.emu, memory_information};

            info.access([&](MEMORY_REGION_INFORMATION64& image_info) {
                memset(&image_info, 0, sizeof(image_info));

                image_info.AllocationBase = region_info.allocation_base;
                image_info.AllocationProtect = map_emulator_to_nt_protection(region_info.initial_permissions);
                // image_info.PartitionId = 0;
                image_info.RegionSize = static_cast<int64_t>(region_info.allocation_length);
                image_info.Reserved = 0x10;
            });

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported memory info class: %X\n", info_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtProtectVirtualMemory(const syscall_context& c, const handle process_handle,
                                           const emulator_object<uint64_t> base_address,
                                           const emulator_object<uint32_t> bytes_to_protect, const uint32_t protection,
                                           const emulator_object<uint32_t> old_protection)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto orig_start = base_address.read();
        const auto orig_length = bytes_to_protect.read();

        const auto aligned_start = page_align_down(orig_start);
        const auto aligned_length = page_align_up(orig_start + orig_length) - aligned_start;

        base_address.write(aligned_start);
        bytes_to_protect.write(static_cast<uint32_t>(aligned_length));

        const auto requested_protection = map_nt_to_emulator_protection(protection);

        c.win_emu.log.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n",
                            aligned_start, aligned_start + aligned_length,
                            get_permission_string(requested_protection).c_str());

        memory_permission old_protection_value{};

        try
        {
            c.win_emu.memory.protect_memory(aligned_start, static_cast<size_t>(aligned_length), requested_protection,
                                            &old_protection_value);
        }
        catch (...)
        {
            return STATUS_INVALID_ADDRESS;
        }

        const auto current_protection = map_emulator_to_nt_protection(old_protection_value);
        old_protection.write(current_protection);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAllocateVirtualMemoryEx(const syscall_context& c, const handle process_handle,
                                              const emulator_object<uint64_t> base_address,
                                              const emulator_object<uint64_t> bytes_to_allocate,
                                              const uint32_t allocation_type, const uint32_t page_protection)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        auto allocation_bytes = bytes_to_allocate.read();
        allocation_bytes = page_align_up(allocation_bytes);
        bytes_to_allocate.write(allocation_bytes);

        const auto protection = map_nt_to_emulator_protection(page_protection);

        auto potential_base = base_address.read();
        if (!potential_base)
        {
            potential_base = c.win_emu.memory.find_free_allocation_base(static_cast<size_t>(allocation_bytes));
        }

        if (!potential_base)
        {
            c.win_emu.log.print(color::dark_gray, "--> Not allocated\n");

            return STATUS_MEMORY_NOT_ALLOCATED;
        }

        base_address.write(potential_base);

        const bool reserve = allocation_type & MEM_RESERVE;
        const bool commit = allocation_type & MEM_COMMIT;

        if ((allocation_type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN | MEM_WRITE_WATCH)) || (!commit && !reserve))
        {
            throw std::runtime_error("Unsupported allocation type!");
        }

        if (commit && !reserve &&
            c.win_emu.memory.commit_memory(potential_base, static_cast<size_t>(allocation_bytes), protection))
        {
            c.win_emu.log.print(is_executable(protection) ? color::gray : color::dark_gray,
                                "--> Committed 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", potential_base,
                                potential_base + allocation_bytes, get_permission_string(protection).c_str());

            return STATUS_SUCCESS;
        }

        c.win_emu.log.print(is_executable(protection) ? color::gray : color::dark_gray,
                            "--> Allocated 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", potential_base,
                            potential_base + allocation_bytes, get_permission_string(protection).c_str());

        return c.win_emu.memory.allocate_memory(potential_base, static_cast<size_t>(allocation_bytes), protection,
                                                !commit)
                   ? STATUS_SUCCESS
                   : STATUS_MEMORY_NOT_ALLOCATED;
    }

    NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, const handle process_handle,
                                            const emulator_object<uint64_t> base_address, const uint64_t /*zero_bits*/,
                                            const emulator_object<uint64_t> bytes_to_allocate,
                                            const uint32_t allocation_type, const uint32_t page_protection)
    {
        return handle_NtAllocateVirtualMemoryEx(c, process_handle, base_address, bytes_to_allocate, allocation_type,
                                                page_protection);
    }

    NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, const handle process_handle,
                                        const emulator_object<uint64_t> base_address,
                                        const emulator_object<uint64_t> bytes_to_allocate, const uint32_t free_type)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto allocation_base = base_address.read();
        const auto allocation_size = bytes_to_allocate.read();

        if (free_type & MEM_RELEASE)
        {
            return c.win_emu.memory.release_memory(allocation_base, static_cast<size_t>(allocation_size))
                       ? STATUS_SUCCESS
                       : STATUS_MEMORY_NOT_ALLOCATED;
        }

        if (free_type & MEM_DECOMMIT)
        {
            return c.win_emu.memory.decommit_memory(allocation_base, static_cast<size_t>(allocation_size))
                       ? STATUS_SUCCESS
                       : STATUS_MEMORY_NOT_ALLOCATED;
        }

        throw std::runtime_error("Bad free type");
    }

    NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, const handle process_handle,
                                        const emulator_pointer base_address, const emulator_pointer buffer,
                                        const ULONG number_of_bytes_to_read,
                                        const emulator_object<ULONG> number_of_bytes_read)
    {
        number_of_bytes_read.write(0);

        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        std::vector<uint8_t> memory{};
        memory.resize(number_of_bytes_to_read);

        if (!c.emu.try_read_memory(base_address, memory.data(), memory.size()))
        {
            return STATUS_INVALID_ADDRESS;
        }

        c.emu.write_memory(buffer, memory.data(), memory.size());
        number_of_bytes_read.write(number_of_bytes_to_read);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetInformationVirtualMemory()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
