#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/io.hpp>

namespace syscalls
{
    // Helper function to parse PE headers and extract image information
    template <typename T>
    static bool parse_pe_headers(const std::vector<std::byte>& file_data, section::image_info& info)
    {
        if (file_data.size() < sizeof(PEDosHeader_t))
        {
            return false;
        }

        const auto* dos_header = reinterpret_cast<const PEDosHeader_t*>(file_data.data());
        if (dos_header->e_magic != PEDosHeader_t::k_Magic)
        {
            return false;
        }

        // First check if we can read up to the optional header magic
        if (file_data.size() < dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t) + sizeof(uint16_t))
        {
            return false;
        }

        // Read the magic number from the optional header
        const auto* magic_ptr =
            reinterpret_cast<const uint16_t*>(file_data.data() + dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t));
        const uint16_t magic = *magic_ptr;

        // Check if the magic matches the expected type
        constexpr uint16_t expected_magic = (sizeof(T) == sizeof(uint32_t))
                                                ? static_cast<uint16_t>(PEOptionalHeader_t<std::uint32_t>::k_Magic)
                                                : static_cast<uint16_t>(PEOptionalHeader_t<std::uint64_t>::k_Magic);

        if (magic != expected_magic)
        {
            return false;
        }

        // Now check the full NT headers size
        if (file_data.size() < dos_header->e_lfanew + sizeof(PENTHeaders_t<T>))
        {
            return false;
        }

        const auto* nt_headers = reinterpret_cast<const PENTHeaders_t<T>*>(file_data.data() + dos_header->e_lfanew);
        if (nt_headers->Signature != PENTHeaders_t<T>::k_Signature)
        {
            return false;
        }

        const auto& file_header = nt_headers->FileHeader;
        const auto& optional_header = nt_headers->OptionalHeader;

        // Extract information from headers
        info.machine = static_cast<uint16_t>(file_header.Machine);
        info.image_characteristics = file_header.Characteristics;

        info.entry_point_rva = optional_header.AddressOfEntryPoint;
        info.image_base = optional_header.ImageBase;
        info.subsystem = optional_header.Subsystem;
        info.subsystem_major_version = optional_header.MajorSubsystemVersion;
        info.subsystem_minor_version = optional_header.MinorSubsystemVersion;
        info.dll_characteristics = optional_header.DllCharacteristics;
        info.size_of_stack_reserve = optional_header.SizeOfStackReserve;
        info.size_of_stack_commit = optional_header.SizeOfStackCommit;
        info.size_of_code = optional_header.SizeOfCode;
        info.loader_flags = optional_header.LoaderFlags;
        info.checksum = optional_header.CheckSum;

        // Check if image contains code
        info.has_code = (optional_header.SizeOfCode > 0) || (optional_header.AddressOfEntryPoint != 0);

        // Also check section characteristics for code sections
        const auto sections_offset = dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t) + file_header.SizeOfOptionalHeader;
        if (file_data.size() >= sections_offset + sizeof(IMAGE_SECTION_HEADER) * file_header.NumberOfSections)
        {
            const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file_data.data() + sections_offset);
            for (uint16_t i = 0; i < file_header.NumberOfSections; ++i)
            {
                if (sections[i].Characteristics & IMAGE_SCN_CNT_CODE)
                {
                    info.has_code = true;
                    break;
                }
            }
        }

        return true;
    }

    NTSTATUS handle_NtCreateSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                    const ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                    const emulator_object<ULARGE_INTEGER> maximum_size, const ULONG section_page_protection,
                                    const ULONG allocation_attributes, const handle file_handle)
    {
        section s{};
        s.section_page_protection = section_page_protection;
        s.allocation_attributes = allocation_attributes;

        const auto* file = c.proc.files.get(file_handle);
        if (file)
        {
            c.win_emu.callbacks.on_generic_access("Section for file", file->name);
            s.file_name = file->name;
        }

        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                auto name = read_unicode_string(c.emu, attributes.ObjectName);
                c.win_emu.callbacks.on_generic_access("Section with name", name);
                s.name = std::move(name);
            }
        }

        if (maximum_size)
        {
            maximum_size.access([&](ULARGE_INTEGER& large_int) {
                large_int.QuadPart = page_align_up(large_int.QuadPart);
                s.maximum_size = large_int.QuadPart;
            });
        }
        else if (!file)
        {
            return STATUS_INVALID_PARAMETER;
        }

        // If this is an image section, parse PE headers
        if ((allocation_attributes & SEC_IMAGE) && !s.file_name.empty())
        {
            std::vector<std::byte> file_data;
            if (utils::io::read_file(s.file_name, &file_data))
            {
                section::image_info info{};

                // Read the PE magic to determine if it's 32-bit or 64-bit
                bool parsed = false;
                if (file_data.size() >= sizeof(PEDosHeader_t))
                {
                    const auto* dos_header = reinterpret_cast<const PEDosHeader_t*>(file_data.data());
                    if (dos_header->e_magic == PEDosHeader_t::k_Magic &&
                        file_data.size() >= dos_header->e_lfanew + sizeof(uint32_t) + sizeof(PEFileHeader_t) + sizeof(uint16_t))
                    {
                        const auto* magic_ptr = reinterpret_cast<const uint16_t*>(file_data.data() + dos_header->e_lfanew +
                                                                                  sizeof(uint32_t) + sizeof(PEFileHeader_t));
                        const uint16_t magic = *magic_ptr;

                        // Parse based on the actual PE type
                        if (magic == PEOptionalHeader_t<std::uint32_t>::k_Magic)
                        {
                            parsed = parse_pe_headers<uint32_t>(file_data, info);
                        }
                        else if (magic == PEOptionalHeader_t<std::uint64_t>::k_Magic)
                        {
                            parsed = parse_pe_headers<uint64_t>(file_data, info);
                        }
                    }
                }

                if (parsed)
                {
                    s.cached_image_info = info;
                }
            }
        }

        const auto h = c.proc.sections.store(std::move(s));
        section_handle.write(h);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenSection(const syscall_context& c, const emulator_object<handle> section_handle,
                                  const ACCESS_MASK /*desired_access*/,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();

        auto filename = read_unicode_string(c.emu, attributes.ObjectName);
        c.win_emu.callbacks.on_generic_access("Opening section", filename);

        if (filename == u"\\Windows\\SharedSection")
        {
            constexpr auto shared_section_size = 0x10000;

            const auto address = c.win_emu.memory.find_free_allocation_base(shared_section_size);
            c.win_emu.memory.allocate_memory(address, shared_section_size, memory_permission::read_write);
            c.proc.shared_section_address = address;
            c.proc.shared_section_size = shared_section_size;

            section_handle.write(SHARED_SECTION);
            return STATUS_SUCCESS;
        }

        if (filename == u"DBWIN_BUFFER")
        {
            constexpr auto dbwin_buffer_section_size = 0x1000;

            const auto address = c.win_emu.memory.find_free_allocation_base(dbwin_buffer_section_size);
            c.win_emu.memory.allocate_memory(address, dbwin_buffer_section_size, memory_permission::read_write);
            c.proc.dbwin_buffer = address;
            c.proc.dbwin_buffer_size = dbwin_buffer_section_size;

            section_handle.write(DBWIN_BUFFER);
            return STATUS_SUCCESS;
        }

        if (filename == u"windows_shell_global_counters"             //
            || filename == u"Global\\__ComCatalogCache__"            //
            || filename == u"{00020000-0000-1005-8005-0000C06B5161}" //
            || filename == u"Global\\{00020000-0000-1005-8005-0000C06B5161}")
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (attributes.RootDirectory != KNOWN_DLLS_DIRECTORY && attributes.RootDirectory != KNOWN_DLLS32_DIRECTORY &&
            attributes.RootDirectory != BASE_NAMED_OBJECTS_DIRECTORY)
        {
            c.win_emu.log.error("Unsupported section\n");
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        utils::string::to_lower_inplace(filename);

        for (auto& section_entry : c.proc.sections)
        {
            if (section_entry.second.is_image() && section_entry.second.name == filename)
            {
                section_handle.write(c.proc.sections.make_handle(section_entry.first));
                return STATUS_SUCCESS;
            }
        }

        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS handle_NtMapViewOfSection(const syscall_context& c, const handle section_handle, const handle process_handle,
                                       const emulator_object<uint64_t> base_address,
                                       const EMULATOR_CAST(EmulatorTraits<Emu64>::ULONG_PTR, ULONG_PTR) /*zero_bits*/,
                                       const EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T) /*commit_size*/,
                                       const emulator_object<LARGE_INTEGER> /*section_offset*/,
                                       const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                       const SECTION_INHERIT /*inherit_disposition*/, const ULONG /*allocation_type*/,
                                       const ULONG /*win32_protect*/)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_handle == SHARED_SECTION)
        {
            const auto shared_section_size = c.proc.shared_section_size;
            const auto address = c.proc.shared_section_address;

            const std::u16string_view windows_dir = c.proc.kusd.get().NtSystemRoot.arr;
            const auto windows_dir_size = windows_dir.size() * 2;

            constexpr auto windows_dir_offset = 0x10;
            c.emu.write_memory(address + 8, windows_dir_offset);

            // aka. BaseStaticServerData (BASE_STATIC_SERVER_DATA)
            const auto obj_address = address + windows_dir_offset;

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> windir_obj{c.emu, obj_address};
            windir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                const auto dir_address = kusd_mmio::address() + offsetof(KUSER_SHARED_DATA64, NtSystemRoot);

                ucs.Buffer = dir_address - obj_address;
                ucs.Length = static_cast<uint16_t>(windows_dir_size);
                ucs.MaximumLength = ucs.Length;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> sysdir_obj{c.emu, windir_obj.value() + windir_obj.size()};
            sysdir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"C:\\WINDOWS\\System32");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> base_dir_obj{c.emu, sysdir_obj.value() + sysdir_obj.size()};
            base_dir_obj.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& ucs) {
                c.proc.base_allocator.make_unicode_string(ucs, u"\\Sessions\\1\\BaseNamedObjects");
                ucs.Buffer = ucs.Buffer - obj_address;
            });

            c.emu.write_memory(obj_address + 0x9C8, 0xFFFFFFFF); // TIME_ZONE_ID_INVALID

            // Windows 2019 offset!
            c.emu.write_memory(obj_address + 0xA70, 0xFFFFFFFF); // TIME_ZONE_ID_INVALID

            if (view_size)
            {
                view_size.write(shared_section_size);
            }

            base_address.write(address);

            return STATUS_SUCCESS;
        }

        if (section_handle == DBWIN_BUFFER)
        {
            const auto dbwin_buffer_section_size = c.proc.dbwin_buffer_size;
            const auto address = c.proc.dbwin_buffer;

            if (view_size)
            {
                view_size.write(dbwin_buffer_section_size);
            }

            base_address.write(address);

            return STATUS_SUCCESS;
        }

        auto* section_entry = c.proc.sections.get(section_handle);
        if (!section_entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (section_entry->is_image())
        {
            const auto* binary = c.win_emu.mod_manager.map_module(section_entry->file_name, c.win_emu.log);
            if (!binary)
            {
                return STATUS_FILE_INVALID;
            }

            std::u16string wide_name(binary->name.begin(), binary->name.end());
            section_entry->name = utils::string::to_lower_consume(wide_name);

            if (view_size.value())
            {
                view_size.write(binary->size_of_image);
            }

            base_address.write(binary->image_base);

            return STATUS_SUCCESS;
        }

        uint64_t size = section_entry->maximum_size;
        std::vector<std::byte> file_data{};

        if (!section_entry->file_name.empty())
        {
            if (!utils::io::read_file(section_entry->file_name, &file_data))
            {
                return STATUS_INVALID_PARAMETER;
            }

            size = page_align_up(file_data.size());
        }

        const auto reserve_only = section_entry->allocation_attributes == SEC_RESERVE;
        const auto protection = map_nt_to_emulator_protection(section_entry->section_page_protection);
        const auto address = c.win_emu.memory.allocate_memory(static_cast<size_t>(size), protection, reserve_only);

        if (!reserve_only && !file_data.empty())
        {
            c.emu.write_memory(address, file_data.data(), file_data.size());
        }

        if (view_size)
        {
            view_size.write(size);
        }

        base_address.write(address);
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtMapViewOfSectionEx(const syscall_context& c, const handle section_handle, const handle process_handle,
                                         const emulator_object<uint64_t> base_address, const emulator_object<LARGE_INTEGER> section_offset,
                                         const emulator_object<EMULATOR_CAST(EmulatorTraits<Emu64>::SIZE_T, SIZE_T)> view_size,
                                         const ULONG allocation_type, const ULONG page_protection,
                                         const uint64_t extended_parameters, // PMEM_EXTENDED_PARAMETER
                                         const ULONG extended_parameter_count)
    {
        // Process extended parameters if present
        struct ExtendedParamsInfo
        {
            uint64_t numa_node = 0;
            uint64_t lowest_address = 0;
            uint64_t highest_address = UINT64_MAX;
            uint64_t alignment = 0;
            uint32_t attribute_flags = 0;
            uint16_t image_machine = IMAGE_FILE_MACHINE_UNKNOWN;
            bool has_address_requirements = false;
            bool has_numa_node = false;
            bool has_attributes = false;
            bool has_image_machine = false;
        } ext_info;

        if (extended_parameters && extended_parameter_count > 0)
        {
            c.win_emu.log.info("NtMapViewOfSectionEx: Processing %u extended parameters\n", extended_parameter_count);

            // Read and process each extended parameter
            for (ULONG i = 0; i < extended_parameter_count; i++)
            {
                const auto param_addr = extended_parameters + (i * sizeof(MEM_EXTENDED_PARAMETER64));
                MEM_EXTENDED_PARAMETER64 param{};

                // Read the extended parameter structure
                if (!c.emu.try_read_memory(param_addr, &param, sizeof(param)))
                {
                    c.win_emu.log.error("NtMapViewOfSectionEx: Failed to read extended parameter %u\n", i);
                    return STATUS_INVALID_PARAMETER;
                }

                // Extract the type (lower 8 bits)
                const auto param_type = static_cast<MEM_EXTENDED_PARAMETER_TYPE>(param.Type & 0xFF);

                switch (param_type)
                {
                case MemExtendedParameterAddressRequirements: {
                    // Read the MEM_ADDRESS_REQUIREMENTS structure
                    MEM_ADDRESS_REQUIREMENTS64 addr_req{};
                    if (!c.emu.try_read_memory(param.Pointer, &addr_req, sizeof(addr_req)))
                    {
                        c.win_emu.log.error("NtMapViewOfSectionEx: Failed to read address requirements\n");
                        return STATUS_INVALID_PARAMETER;
                    }

                    ext_info.lowest_address = addr_req.LowestStartingAddress;
                    ext_info.highest_address = addr_req.HighestEndingAddress;
                    ext_info.alignment = addr_req.Alignment;
                    ext_info.has_address_requirements = true;

                    c.win_emu.log.info("NtMapViewOfSectionEx: Address requirements - Low: 0x%" PRIX64 ", High: 0x%" PRIX64
                                       ", Align: 0x%" PRIX64 "\n",
                                       ext_info.lowest_address, ext_info.highest_address, ext_info.alignment);
                }
                break;

                case MemExtendedParameterNumaNode:
                    ext_info.numa_node = param.ULong64;
                    ext_info.has_numa_node = true;
                    c.win_emu.log.info("NtMapViewOfSectionEx: NUMA node: %" PRIu64 "\n", ext_info.numa_node);
                    break;

                case MemExtendedParameterAttributeFlags:
                    ext_info.attribute_flags = static_cast<uint32_t>(param.ULong64);
                    ext_info.has_attributes = true;
                    c.win_emu.log.info("NtMapViewOfSectionEx: Attribute flags: 0x%X\n", ext_info.attribute_flags);

                    // Log specific attribute flags
                    if (ext_info.attribute_flags & MEM_EXTENDED_PARAMETER_GRAPHICS)
                    {
                        c.win_emu.log.info("  - Graphics memory requested\n");
                    }
                    if (ext_info.attribute_flags & MEM_EXTENDED_PARAMETER_NONPAGED)
                    {
                        c.win_emu.log.info("  - Non-paged memory requested\n");
                    }
                    if (ext_info.attribute_flags & MEM_EXTENDED_PARAMETER_EC_CODE)
                    {
                        c.win_emu.log.info("  - EC code memory requested\n");
                    }
                    break;

                case MemExtendedParameterImageMachine:
                    ext_info.image_machine = static_cast<uint16_t>(param.ULong);
                    ext_info.has_image_machine = true;
                    c.win_emu.log.info("NtMapViewOfSectionEx: Image machine: 0x%X\n", ext_info.image_machine);
                    break;

                case MemExtendedParameterPartitionHandle:
                    c.win_emu.log.info("NtMapViewOfSectionEx: Partition handle parameter (not supported)\n");
                    break;

                case MemExtendedParameterUserPhysicalHandle:
                    c.win_emu.log.info("NtMapViewOfSectionEx: User physical handle parameter (not supported)\n");
                    break;

                default:
                    c.win_emu.log.warn("NtMapViewOfSectionEx: Unknown extended parameter type: %u\n", param_type);
                    break;
                }
            }

            // Store extended parameters info in process context for other syscalls to use
            // This allows NtAllocateVirtualMemoryEx and other functions to access the same info
            c.proc.last_extended_params_numa_node = ext_info.numa_node;
            c.proc.last_extended_params_attributes = ext_info.attribute_flags;
        }

        // Call the existing NtMapViewOfSection implementation
        // For WOW64 processes with image machine parameter, validate architecture compatibility
        if (ext_info.has_image_machine && c.proc.is_wow64_process)
        {
            c.win_emu.log.info("NtMapViewOfSectionEx: WOW64 process mapping with machine type 0x%X\n", ext_info.image_machine);

            // Special handling for IMAGE_FILE_MACHINE_I386 (0x014c) on WOW64
            if (ext_info.image_machine == IMAGE_FILE_MACHINE_I386)
            {
                // This indicates the caller wants to map a 32-bit view
                // Store this for the module manager to use
                c.win_emu.log.info("NtMapViewOfSectionEx: Mapping 32-bit view for WOW64 process\n");
            }
            else if (ext_info.image_machine == IMAGE_FILE_MACHINE_AMD64)
            {
                // This indicates the caller wants to map a 64-bit view
                c.win_emu.log.info("NtMapViewOfSectionEx: Mapping 64-bit view for WOW64 process\n");
            }
        }

        // Store extended parameters for other syscalls to use
        if (ext_info.has_numa_node)
        {
            c.proc.last_extended_params_numa_node = ext_info.numa_node;
        }
        if (ext_info.has_attributes)
        {
            c.proc.last_extended_params_attributes = ext_info.attribute_flags;
        }
        if (ext_info.has_image_machine)
        {
            c.proc.last_extended_params_image_machine = ext_info.image_machine;
        }

        // Perform the actual mapping
        const auto status = handle_NtMapViewOfSection(c, section_handle, process_handle, base_address,
                                                      0,                // zero_bits (not in Ex)
                                                      0,                // commit_size (not in Ex)
                                                      section_offset,   // section_offset
                                                      view_size,        // view_size
                                                      ViewUnmap,        // inherit_disposition (default)
                                                      allocation_type,  // allocation_type
                                                      page_protection); // page_protection

        // If mapping succeeded and this is a WOW64 image section with specific machine type
        if (NT_SUCCESS(status) && ext_info.has_image_machine && c.proc.is_wow64_process)
        {
            // Check if this was an image section (DLL/EXE)
            auto* section_entry = c.proc.sections.get(section_handle);
            if (section_entry && section_entry->is_image())
            {
                c.win_emu.log.info("NtMapViewOfSectionEx: Successfully mapped image section for WOW64 with machine type 0x%X\n",
                                   ext_info.image_machine);
                // Note: In a full WOW64 implementation, we would check for Wow64Transition export here
            }
        }

        return status;
    }

    NTSTATUS handle_NtUnmapViewOfSection(const syscall_context& c, const handle process_handle, const uint64_t base_address)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (!base_address)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (base_address == c.proc.shared_section_address)
        {
            c.proc.shared_section_address = 0;
            c.win_emu.memory.release_memory(base_address, static_cast<size_t>(c.proc.shared_section_size));
            return STATUS_SUCCESS;
        }

        if (base_address == c.proc.dbwin_buffer)
        {
            c.proc.dbwin_buffer = 0;
            c.win_emu.memory.release_memory(base_address, static_cast<size_t>(c.proc.dbwin_buffer_size));
            return STATUS_SUCCESS;
        }

        const auto* mod = c.win_emu.mod_manager.find_by_address(base_address);
        if (mod != nullptr)
        {
            if (c.win_emu.mod_manager.unmap(base_address))
            {
                return STATUS_SUCCESS;
            }

            return STATUS_INVALID_PARAMETER;
        }

        if (c.win_emu.memory.release_memory(base_address, 0))
        {
            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unmapping non-module/non-memory section not supported!\n");
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUnmapViewOfSectionEx(const syscall_context& c, const handle process_handle, const uint64_t base_address,
                                           const ULONG /*flags*/)
    {
        return handle_NtUnmapViewOfSection(c, process_handle, base_address);
    }

    NTSTATUS handle_NtAreMappedFilesTheSame()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQuerySection(const syscall_context& c, const handle section_handle,
                                   const SECTION_INFORMATION_CLASS section_information_class, const uint64_t section_information,
                                   const EmulatorTraits<Emu64>::SIZE_T section_information_length,
                                   const emulator_object<EmulatorTraits<Emu64>::SIZE_T> result_length)
    {
        // Check if section handle is valid
        auto* section_entry = c.proc.sections.get(section_handle);

        // Handle special sections
        if (section_handle == SHARED_SECTION || section_handle == DBWIN_BUFFER)
        {
            // These special sections don't support querying
            return STATUS_INVALID_HANDLE;
        }

        if (!section_entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        switch (section_information_class)
        {
        case SECTION_INFORMATION_CLASS::SectionBasicInformation: {
            // Check buffer size
            if (section_information_length < sizeof(SECTION_BASIC_INFORMATION<EmulatorTraits<Emu64>>))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            SECTION_BASIC_INFORMATION<EmulatorTraits<Emu64>> info{};

            // BaseAddress - typically NULL unless SEC_BASED is used
            info.BaseAddress = 0;

            // Attributes - combine the SEC_ flags
            info.Attributes = section_entry->allocation_attributes;

            // If it's an image section, ensure SEC_IMAGE is set
            if (section_entry->is_image())
            {
                info.Attributes |= SEC_IMAGE;
            }

            // If it's file-backed, ensure SEC_FILE is set
            if (!section_entry->file_name.empty())
            {
                info.Attributes |= SEC_FILE;
            }

            // Size - maximum size of the section
            info.Size.QuadPart = static_cast<LONGLONG>(section_entry->maximum_size);

            // Write the structure to user buffer
            c.emu.write_memory(section_information, &info, sizeof(info));

            // Set return length if requested
            if (result_length)
            {
                result_length.write(sizeof(SECTION_BASIC_INFORMATION<EmulatorTraits<Emu64>>));
            }

            return STATUS_SUCCESS;
        }

        case SECTION_INFORMATION_CLASS::SectionImageInformation: {
            // Only image sections support this query
            if (!section_entry->is_image())
            {
                return STATUS_SECTION_NOT_IMAGE;
            }

            // Check buffer size
            if (section_information_length < sizeof(SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>> info{};

            // First check if we have cached PE information
            if (section_entry->cached_image_info.has_value())
            {
                const auto& cached = section_entry->cached_image_info.value();

                // TransferAddress - entry point address (image base + RVA)
                info.TransferAddress = static_cast<std::uint64_t>(cached.image_base + cached.entry_point_rva);

                // Machine type
                info.Machine = static_cast<PEMachineType>(cached.machine);

                // Subsystem information
                info.SubSystemType = cached.subsystem;
                info.SubSystemMajorVersion = cached.subsystem_major_version;
                info.SubSystemMinorVersion = cached.subsystem_minor_version;

                // Stack sizes
                info.MaximumStackSize = cached.size_of_stack_reserve;
                info.CommittedStackSize = cached.size_of_stack_commit;

                // Image characteristics
                info.ImageCharacteristics = cached.image_characteristics;
                info.DllCharacteristics = cached.dll_characteristics;

                // Image contains code
                info.ImageContainsCode = cached.has_code ? TRUE : FALSE;

                // Image flags
                info.ImageMappedFlat = 1;
                info.ImageDynamicallyRelocated = (cached.dll_characteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) ? 1 : 0;

                // Other fields
                info.ZeroBits = 0;
                info.LoaderFlags = cached.loader_flags;
                info.CheckSum = cached.checksum;
                info.ImageFileSize = static_cast<ULONG>(section_entry->maximum_size);
            }
            else
            {
                // Try to get the mapped module to extract PE information
                // Convert u16string to string for find_by_name
                std::string narrow_name;
                if (!section_entry->name.empty())
                {
                    narrow_name = u16_to_u8(section_entry->name);
                }
                else if (!section_entry->file_name.empty())
                {
                    narrow_name = u16_to_u8(section_entry->file_name);
                }

                const mapped_module* module = nullptr;
                if (!narrow_name.empty())
                {
                    module = c.win_emu.mod_manager.find_by_name(narrow_name);
                }

                if (module)
                {
                    // TransferAddress - entry point address
                    info.TransferAddress = static_cast<std::uint64_t>(module->entry_point);

                    // Machine type and other fields would need to be extracted from PE headers
                    // For now, set reasonable defaults for x64
                    info.Machine = PEMachineType::AMD64;
                    info.SubSystemType = 3; // IMAGE_SUBSYSTEM_WINDOWS_CUI
                    info.SubSystemMajorVersion = 10;
                    info.SubSystemMinorVersion = 0;

                    // Stack sizes - typical defaults
                    info.MaximumStackSize = 0x100000;  // 1MB
                    info.CommittedStackSize = 0x10000; // 64KB

                    // Image characteristics
                    info.ImageCharacteristics = 0x0022; // IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE
                    info.DllCharacteristics = 0x8160;   // Common DLL characteristics including ASLR and DEP

                    // Check if it's a DLL
                    if (section_entry->name.find(u".dll") != std::u16string::npos)
                    {
                        info.ImageCharacteristics |= IMAGE_FILE_DLL;
                    }

                    // Image contains code
                    info.ImageContainsCode = TRUE;

                    // Image flags
                    info.ImageMappedFlat = 1;
                    info.ImageDynamicallyRelocated = 1; // ASLR enabled

                    // File size
                    info.ImageFileSize = static_cast<ULONG>(module->size_of_image);

                    // Other fields
                    info.ZeroBits = 0;
                    info.LoaderFlags = 0;
                    info.CheckSum = 0;
                }
                else
                {
                    // If module is not mapped yet and no cached info, return minimal information
                    info.Machine = PEMachineType::AMD64;
                    info.SubSystemType = 3;
                    info.SubSystemMajorVersion = 10;
                    info.SubSystemMinorVersion = 0;
                    info.MaximumStackSize = 0x100000;
                    info.CommittedStackSize = 0x10000;
                    info.ImageCharacteristics = 0x0022;
                    info.DllCharacteristics = 0x8160;
                    info.ImageContainsCode = TRUE;
                    info.ImageMappedFlat = 1;
                    info.ImageDynamicallyRelocated = 1;
                    info.ImageFileSize = static_cast<ULONG>(section_entry->maximum_size);
                }
            }

            // Write the structure to user buffer
            c.emu.write_memory(section_information, &info, sizeof(info));

            // Set return length if requested
            if (result_length)
            {
                result_length.write(sizeof(SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>));
            }

            return STATUS_SUCCESS;
        }

        case SECTION_INFORMATION_CLASS::SectionRelocationInformation:
        case SECTION_INFORMATION_CLASS::SectionOriginalBaseInformation:
        case SECTION_INFORMATION_CLASS::SectionInternalImageInformation:
            // These information classes are not implemented
            return STATUS_NOT_SUPPORTED;

        default:
            return STATUS_NOT_SUPPORTED;
        }
    }
}
