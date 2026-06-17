#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../memory_manager.hpp"

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            struct allocation_address_requirements
            {
                uint64_t lowest_address = MIN_ALLOCATION_ADDRESS;
                uint64_t highest_address = MAX_ALLOCATION_END_EXCL - 1;
                uint64_t alignment = ALLOCATION_GRANULARITY;
                bool present = false;
                bool has_non_default_values = false;
            };

            bool is_power_of_two(const uint64_t value)
            {
                return value != 0 && (value & (value - 1)) == 0;
            }

            std::optional<uint64_t> checked_add(const uint64_t lhs, const uint64_t rhs)
            {
                if (lhs > UINT64_MAX - rhs)
                {
                    return std::nullopt;
                }

                return lhs + rhs;
            }

            NTSTATUS parse_allocation_address_requirements(const syscall_context& c,
                                                           const emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                                           const ULONG extended_parameter_count,
                                                           allocation_address_requirements& requirements)
            {
                if (!extended_parameters)
                {
                    return extended_parameter_count == 0 ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
                }

                for (ULONG i = 0; i < extended_parameter_count; ++i)
                {
                    const auto param = extended_parameters.try_read(i);
                    if (!param.has_value())
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const auto param_type = static_cast<MEM_EXTENDED_PARAMETER_TYPE>(param->Type & 0xFF);
                    if (param_type != MemExtendedParameterAddressRequirements)
                    {
                        continue;
                    }

                    if (requirements.present)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    if (!param->Pointer)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    const emulator_object<MEM_ADDRESS_REQUIREMENTS64> address_requirements{c.emu, param->Pointer};
                    const auto req = address_requirements.try_read();
                    if (!req.has_value())
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    if (req->LowestStartingAddress != 0 &&
                        (req->LowestStartingAddress < MIN_ALLOCATION_ADDRESS || req->LowestStartingAddress % ALLOCATION_GRANULARITY != 0))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    if (req->HighestEndingAddress != 0)
                    {
                        if (req->HighestEndingAddress > MAX_ALLOCATION_ADDRESS)
                        {
                            return STATUS_INVALID_PARAMETER;
                        }

                        const auto highest_end_plus_one = req->HighestEndingAddress + 1;
                        if (highest_end_plus_one % ALLOCATION_GRANULARITY != 0)
                        {
                            return STATUS_INVALID_PARAMETER;
                        }
                    }

                    if (req->Alignment != 0 && (!is_power_of_two(req->Alignment) || req->Alignment < ALLOCATION_GRANULARITY))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }

                    requirements.present = true;
                    requirements.has_non_default_values =
                        req->LowestStartingAddress != 0 || req->HighestEndingAddress != 0 || req->Alignment != 0;
                    requirements.lowest_address = req->LowestStartingAddress ? req->LowestStartingAddress : MIN_ALLOCATION_ADDRESS;
                    requirements.highest_address = req->HighestEndingAddress ? req->HighestEndingAddress : MAX_ALLOCATION_END_EXCL - 1;
                    requirements.alignment = req->Alignment ? req->Alignment : ALLOCATION_GRANULARITY;

                    if (requirements.lowest_address > requirements.highest_address)
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                }

                return STATUS_SUCCESS;
            }

            std::optional<std::u16string> get_mapped_filename(const syscall_context& c, const uint64_t base_address)
            {
                const auto* mod = c.win_emu.mod_manager.find_by_address(base_address);
                if (!mod || mod->module_path.empty())
                {
                    return std::nullopt;
                }

                try
                {
                    return mod->module_path.to_device_path();
                }
                catch (const std::exception&)
                {
                    return mod->module_path.to_unc_path();
                }
            }
        }

        NTSTATUS handle_NtQueryVirtualMemory(const syscall_context& c, const handle process_handle, const uint64_t base_address,
                                             const uint32_t info_class, const uint64_t memory_information,
                                             const uint64_t memory_information_length, const emulator_object<uint64_t> return_length)
        {
            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (info_class == MemoryWorkingSetExInformation || info_class == MemoryImageExtensionInformation)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (base_address < MIN_ALLOCATION_ADDRESS || base_address >= MAX_ALLOCATION_END_EXCL)
            {
                if (return_length)
                {
                    return_length.write(0);
                }
                return STATUS_INVALID_PARAMETER;
            }

            // https://www.exploit-db.com/exploits/44464
            // Both information classes appear to return the same output structure, MEMORY_BASIC_INFORMATION
            if (info_class == MemoryBasicInformation || info_class == MemoryPrivilegedBasicInformation)
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

                    if (!region_info.is_reserved)
                    {
                        image_info.Type = 0;
                    }
                    else
                    {
                        image_info.Type = memory_region_policy::to_memory_basic_information_type(region_info.kind);
                    }
                });

                return STATUS_SUCCESS;
            }

            if (info_class == MemoryMappedFilenameInformation)
            {
                const auto mapped_filename = get_mapped_filename(c, base_address);
                if (!mapped_filename)
                {
                    return STATUS_INVALID_ADDRESS;
                }

                const auto string_bytes = static_cast<uint32_t>(mapped_filename->size() * sizeof(char16_t));
                const auto required_length =
                    static_cast<uint32_t>(sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + string_bytes + sizeof(char16_t));

                if (return_length)
                {
                    return_length.write(required_length);
                }

                if (memory_information_length < required_length)
                {
                    return STATUS_BUFFER_OVERFLOW;
                }

                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> info{c.emu, memory_information};
                info.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& filename) {
                    const auto buffer_start = static_cast<uint64_t>(memory_information) + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
                    filename.Length = static_cast<USHORT>(string_bytes);
                    filename.MaximumLength = static_cast<USHORT>(string_bytes + sizeof(char16_t));
                    filename.Buffer = buffer_start;

                    c.emu.write_memory(buffer_start, mapped_filename->data(), string_bytes);
                    const char16_t terminator = u'\0';
                    c.emu.write_memory(buffer_start + string_bytes, &terminator, sizeof(terminator));
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
            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            const auto orig_start = base_address.read();
            const auto orig_length = bytes_to_protect.read();

            const auto aligned_start = page_align_down(orig_start);
            const auto aligned_length = page_align_up(orig_start + orig_length) - aligned_start;

            base_address.write(aligned_start);
            bytes_to_protect.write(static_cast<uint32_t>(aligned_length));

            const auto requested_protection = try_map_nt_to_emulator_protection(protection);
            if (!requested_protection.has_value())
            {
                return STATUS_INVALID_PAGE_PROTECTION;
            }

            c.win_emu.callbacks.on_memory_protect(aligned_start, aligned_length, *requested_protection);

            nt_memory_permission old_protection_value{};

            try
            {
                c.win_emu.memory.protect_memory(aligned_start, static_cast<size_t>(aligned_length), *requested_protection,
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
                                                  const emulator_object<uint64_t> bytes_to_allocate, const uint32_t allocation_type,
                                                  const uint32_t page_protection,
                                                  const emulator_object<MEM_EXTENDED_PARAMETER64> extended_parameters,
                                                  const ULONG extended_parameter_count)
        {
            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            auto allocation_bytes = bytes_to_allocate.read();

            if (allocation_bytes == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto requested_base = base_address.read();
            const auto requested_allocation_bytes = allocation_bytes;

            allocation_bytes = page_align_up(allocation_bytes);
            bytes_to_allocate.write(allocation_bytes);

            const auto base_protection = page_protection & ~static_cast<uint32_t>(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
            if (base_protection == PAGE_WRITECOPY || base_protection == PAGE_EXECUTE_WRITECOPY)
            {
                return STATUS_INVALID_PAGE_PROTECTION;
            }

            const auto protection = try_map_nt_to_emulator_protection(page_protection);
            if (!protection.has_value())
            {
                return STATUS_INVALID_PAGE_PROTECTION;
            }

            if (allocation_type & MEM_RESET)
            {
                if (allocation_type & ~MEM_RESET)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                if (requested_base != page_align_down(requested_base))
                {
                    return STATUS_CONFLICTING_ADDRESSES;
                }

                if (requested_allocation_bytes != page_align_up(requested_allocation_bytes))
                {
                    return STATUS_CONFLICTING_ADDRESSES;
                }

                const auto reset_end = checked_add(requested_base, requested_allocation_bytes);
                if (!reset_end.has_value())
                {
                    return STATUS_CONFLICTING_ADDRESSES;
                }

                const auto reset_base = requested_base;
                const auto reset_size = requested_allocation_bytes;

                const auto region_info = c.win_emu.memory.get_region_info(reset_base);
                const auto is_valid_kind = region_info.kind == memory_region_kind::private_allocation ||
                                           region_info.kind == memory_region_kind::pagefile_section_view;
                if (!region_info.is_reserved || !is_valid_kind || region_info.allocation_base > reset_base)
                {
                    return STATUS_MEMORY_NOT_ALLOCATED;
                }

                const auto allocation_end = checked_add(region_info.allocation_base, region_info.allocation_length);
                if (!allocation_end.has_value() || *allocation_end < *reset_end)
                {
                    return STATUS_MEMORY_NOT_ALLOCATED;
                }

                base_address.write(reset_base);
                bytes_to_allocate.write(reset_size);

                // Real Windows may discard page contents, we just return success.
                return STATUS_SUCCESS;
            }

            allocation_address_requirements address_requirements{};
            const auto parse_status =
                parse_allocation_address_requirements(c, extended_parameters, extended_parameter_count, address_requirements);
            if (parse_status != STATUS_SUCCESS)
            {
                return parse_status;
            }

            if (requested_base != 0 && address_requirements.present && address_requirements.has_non_default_values)
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto potential_base = requested_base;
            if (!potential_base)
            {
                potential_base =
                    c.win_emu.memory.find_free_allocation_base(static_cast<size_t>(allocation_bytes), 0, address_requirements.alignment,
                                                               address_requirements.lowest_address, address_requirements.highest_address);
            }
            else
            {
                // https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntallocatevirtualmemory
                // BaseAddress
                // A pointer to a variable that will receive the base address of the allocated region of pages. If the
                // initial value of BaseAddress is non-NULL, the region is allocated starting at the specified virtual
                // address rounded down to the next host page size address boundary. If the initial value of BaseAddress
                // is NULL, the operating system will determine where to allocate the region.
                potential_base = page_align_down(potential_base);
            }

            if (!potential_base)
            {
                return STATUS_MEMORY_NOT_ALLOCATED;
            }

            base_address.write(potential_base);

            const bool reserve = allocation_type & MEM_RESERVE;
            const bool commit = allocation_type & MEM_COMMIT;

            if ((allocation_type & ~(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN | MEM_WRITE_WATCH)) || (!commit && !reserve))
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (commit && !reserve && c.win_emu.memory.commit_memory(potential_base, static_cast<size_t>(allocation_bytes), *protection))
            {
                c.win_emu.callbacks.on_memory_allocate(potential_base, allocation_bytes, *protection, true);
                return STATUS_SUCCESS;
            }

            c.win_emu.callbacks.on_memory_allocate(potential_base, allocation_bytes, *protection, false);

            return c.win_emu.memory.allocate_memory(potential_base, static_cast<size_t>(allocation_bytes), *protection, !commit)
                       ? STATUS_SUCCESS
                       : STATUS_MEMORY_NOT_ALLOCATED;
        }

        NTSTATUS handle_NtAllocateVirtualMemory(const syscall_context& c, const handle process_handle,
                                                const emulator_object<uint64_t> base_address, const uint64_t /*zero_bits*/,
                                                const emulator_object<uint64_t> bytes_to_allocate, const uint32_t allocation_type,
                                                const uint32_t page_protection)
        {
            return handle_NtAllocateVirtualMemoryEx(c, process_handle, base_address, bytes_to_allocate, allocation_type, page_protection,
                                                    emulator_object<MEM_EXTENDED_PARAMETER64>{c.emu}, 0);
        }

        NTSTATUS handle_NtFreeVirtualMemory(const syscall_context& c, const handle process_handle,
                                            const emulator_object<uint64_t> base_address, const emulator_object<uint64_t> bytes_to_allocate,
                                            const uint32_t free_type)
        {
            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (free_type == 0)
            {
                return STATUS_INVALID_PARAMETER_4;
            }

            const auto allocation_base = base_address.read();
            const auto allocation_size = bytes_to_allocate.read();

            if (free_type & MEM_RELEASE)
            {
                if (allocation_size)
                {
                    return STATUS_INVALID_PARAMETER;
                }

                // The base is rounded down to a page; the rounded base must equal the region's base.
                const auto release_base = page_align_down(allocation_base);

                const auto region_info = c.win_emu.memory.get_region_info(release_base);
                if (!region_info.is_reserved)
                {
                    return STATUS_MEMORY_NOT_ALLOCATED;
                }

                if (region_info.allocation_base != release_base)
                {
                    return STATUS_FREE_VM_NOT_AT_BASE;
                }

                const auto region_kind = c.win_emu.memory.get_region_kind(release_base);
                const auto denied_status = memory_region_policy::nt_free_virtual_memory_denied_status(region_kind);
                if (denied_status != STATUS_SUCCESS)
                {
                    return denied_status;
                }

                const auto released_length = region_info.allocation_length;
                const bool success = c.win_emu.memory.release_memory(release_base, 0);
                if (success)
                {
                    base_address.write(release_base);
                    bytes_to_allocate.write(static_cast<uint64_t>(released_length));
                    return STATUS_SUCCESS;
                }

                const auto post_release_region_info = c.win_emu.memory.get_region_info(release_base);
                if (!post_release_region_info.is_reserved)
                {
                    return STATUS_MEMORY_NOT_ALLOCATED;
                }

                return STATUS_FREE_VM_NOT_AT_BASE;
            }

            if (free_type & MEM_DECOMMIT)
            {
                const auto region_kind = c.win_emu.memory.get_region_kind(allocation_base);
                const auto denied_status = memory_region_policy::nt_free_virtual_memory_denied_status(region_kind);
                if (denied_status != STATUS_SUCCESS)
                {
                    return denied_status;
                }

                auto decommit_base = allocation_base;
                auto decommit_size = static_cast<size_t>(allocation_size);
                if (!decommit_size)
                {
                    const auto region_info = c.win_emu.memory.get_region_info(allocation_base);
                    if (!region_info.is_reserved)
                    {
                        return STATUS_MEMORY_NOT_ALLOCATED;
                    }

                    if (region_info.allocation_base != allocation_base)
                    {
                        return STATUS_FREE_VM_NOT_AT_BASE;
                    }

                    decommit_size = region_info.allocation_length;
                }
                else
                {
                    // NtFreeVirtualMemory decommits whole pages: round the base down and the end up.
                    decommit_base = page_align_down(allocation_base);
                    decommit_size = static_cast<size_t>(page_align_up(allocation_base + decommit_size) - decommit_base);
                }

                const bool success = c.win_emu.memory.decommit_memory(decommit_base, decommit_size);
                if (!success)
                {
                    return STATUS_MEMORY_NOT_ALLOCATED;
                }

                base_address.write(decommit_base);
                bytes_to_allocate.write(static_cast<uint64_t>(decommit_size));
                return STATUS_SUCCESS;
            }

            return STATUS_INVALID_PARAMETER_4;
        }

        NTSTATUS handle_NtReadVirtualMemory(const syscall_context& c, const handle process_handle, const emulator_pointer base_address,
                                            const emulator_pointer buffer, const ULONG number_of_bytes_to_read,
                                            const emulator_object<ULONG> number_of_bytes_read)
        {
            number_of_bytes_read.try_write(0);

            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (number_of_bytes_to_read == 0)
            {
                return STATUS_SUCCESS;
            }

            constexpr size_t page_size = 0x1000;
            const auto bytes_until_page_boundary = [](const uint64_t address) {
                const auto offset = address % page_size;
                return static_cast<size_t>(offset == 0 ? page_size : page_size - offset);
            };

            std::vector<uint8_t> memory(page_size, 0);
            size_t bytes_read = 0;
            while (bytes_read < number_of_bytes_to_read)
            {
                const auto current_base = static_cast<uint64_t>(base_address) + bytes_read;
                const auto current_buffer = static_cast<uint64_t>(buffer) + bytes_read;
                const auto bytes_remaining = static_cast<size_t>(number_of_bytes_to_read) - bytes_read;
                const auto chunk_size =
                    std::min({bytes_remaining, bytes_until_page_boundary(current_base), bytes_until_page_boundary(current_buffer)});

                if (!c.emu.try_read_memory(current_base, memory.data(), chunk_size))
                {
                    break;
                }

                if (!c.emu.try_write_memory(current_buffer, memory.data(), chunk_size))
                {
                    break;
                }

                bytes_read += chunk_size;
            }

            number_of_bytes_read.try_write(static_cast<ULONG>(bytes_read));
            if (bytes_read == number_of_bytes_to_read)
            {
                return STATUS_SUCCESS;
            }

            return bytes_read == 0 ? STATUS_INVALID_ADDRESS : STATUS_PARTIAL_COPY;
        }

        NTSTATUS handle_NtWriteVirtualMemory(const syscall_context& c, const handle process_handle, const emulator_pointer base_address,
                                             const emulator_pointer buffer, const ULONG number_of_bytes_to_write,
                                             const emulator_object<ULONG> number_of_bytes_write)
        {
            number_of_bytes_write.try_write(0);

            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            std::vector<uint8_t> memory(number_of_bytes_to_write, 0);

            if (!c.emu.try_read_memory(buffer, memory.data(), number_of_bytes_to_write))
            {
                return STATUS_INVALID_ADDRESS;
            }

            if (!c.emu.try_write_memory(base_address, memory.data(), number_of_bytes_to_write))
            {
                return STATUS_INVALID_ADDRESS;
            }

            number_of_bytes_write.try_write(number_of_bytes_to_write);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetInformationVirtualMemory()
        {
            return STATUS_NOT_SUPPORTED;
        }

        BOOL handle_NtLockVirtualMemory()
        {
            return TRUE;
        }

        NTSTATUS handle_NtUnlockVirtualMemory()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtFlushVirtualMemory(const syscall_context& c, const handle process_handle,
                                             const emulator_object<uint64_t> base_address, const emulator_object<uint64_t> region_size,
                                             const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status_block)
        {
            if (!c.proc.is_current_process_handle(process_handle))
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (!base_address || !region_size)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            const auto address = base_address.read();
            const auto size = region_size.read();

            if (!address || !size)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto aligned_start = page_align_down(address);
            const auto aligned_end = page_align_up(address + size);

            if (aligned_end <= aligned_start)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto region_info = c.win_emu.memory.get_region_info(aligned_start);
            if (!region_info.is_committed || region_info.start > aligned_start || region_info.start + region_info.length < aligned_end)
            {
                return STATUS_INVALID_PARAMETER;
            }

            base_address.write(aligned_start);
            region_size.write(aligned_end - aligned_start);

            if (io_status_block)
            {
                IO_STATUS_BLOCK<EmulatorTraits<Emu64>> block{};
                block.Status = STATUS_SUCCESS;
                block.Information = aligned_end - aligned_start;
                io_status_block.write(block);
            }

            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
