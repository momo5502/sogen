#include "../std_include.hpp"
#include "module_mapping.hpp"
#include <address_utils.hpp>

#include <utils/io.hpp>
#include <utils/buffer_accessor.hpp>
#include <platform/win_pefile.hpp>

#include <iostream>

namespace
{
    template <typename T>
    std::vector<std::byte> read_mapped_memory(const memory_manager& memory, const mapped_module& binary)
    {
        std::vector<std::byte> mem{};
        mem.resize(static_cast<size_t>(binary.size_of_image));
        memory.read_memory(binary.image_base, mem.data(), mem.size());

        return mem;
    }

    template <typename T>
    void collect_imports(mapped_module& binary, const utils::safe_buffer_accessor<const std::byte> buffer,
                         const PEOptionalHeader_t<T>& optional_header)
    {
        const auto& import_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (import_directory_entry.VirtualAddress == 0 || import_directory_entry.Size == 0)
        {
            return;
        }

        const auto import_descriptors = buffer.as<IMAGE_IMPORT_DESCRIPTOR>(import_directory_entry.VirtualAddress);

        for (size_t i = 0;; ++i)
        {
            const auto descriptor = import_descriptors.get(i);
            if (!descriptor.Name)
            {
                break;
            }

            // Use architecture-specific thunk data type
            using thunk_traits = thunk_data_traits<T>;
            using thunk_type = typename thunk_traits::type;

            const auto module_index = binary.imported_modules.size();
            binary.imported_modules.push_back(buffer.as_string(descriptor.Name));

            auto original_thunk_data = buffer.as<thunk_type>(descriptor.FirstThunk);
            if (descriptor.OriginalFirstThunk)
            {
                original_thunk_data = buffer.as<thunk_type>(descriptor.OriginalFirstThunk);
            }

            for (size_t j = 0;; ++j)
            {
                const auto original_thunk = original_thunk_data.get(j);
                if (!original_thunk.u1.AddressOfData)
                {
                    break;
                }

                static_assert(sizeof(thunk_type) == sizeof(T));
                const auto thunk_rva = descriptor.FirstThunk + sizeof(thunk_type) * j;
                const auto thunk_address = thunk_rva + binary.image_base;

                auto& sym = binary.imports[thunk_address];
                sym.module_index = module_index;

                // Use architecture-specific ordinal checking
                if (thunk_traits::snap_by_ordinal(original_thunk.u1.Ordinal))
                {
                    sym.name = "#" + std::to_string(thunk_traits::ordinal_mask(original_thunk.u1.Ordinal));
                }
                else
                {
                    sym.name =
                        buffer.as_string(static_cast<size_t>(original_thunk.u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name)));
                }
            }
        }
    }

    template <typename T>
    void collect_exports(mapped_module& binary, const utils::safe_buffer_accessor<const std::byte> buffer,
                         const PEOptionalHeader_t<T>& optional_header)
    {
        const auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (export_directory_entry.VirtualAddress == 0 || export_directory_entry.Size == 0)
        {
            return;
        }

        const auto export_directory = buffer.as<IMAGE_EXPORT_DIRECTORY>(export_directory_entry.VirtualAddress).get();

        const auto names_count = export_directory.NumberOfNames;
        // const auto function_count = export_directory.NumberOfFunctions;

        const auto names = buffer.as<DWORD>(export_directory.AddressOfNames);
        const auto ordinals = buffer.as<WORD>(export_directory.AddressOfNameOrdinals);
        const auto functions = buffer.as<DWORD>(export_directory.AddressOfFunctions);

        binary.exports.reserve(names_count);

        for (DWORD i = 0; i < names_count; i++)
        {
            const auto ordinal = ordinals.get(i);

            exported_symbol symbol{};
            symbol.ordinal = export_directory.Base + ordinal;
            symbol.rva = functions.get(ordinal);
            symbol.address = binary.image_base + symbol.rva;
            symbol.name = buffer.as_string(names.get(i));

            binary.exports.push_back(std::move(symbol));
        }

        for (const auto& symbol : binary.exports)
        {
            binary.address_names.try_emplace(symbol.address, symbol.name);
        }
    }

    template <typename T>
        requires(std::is_integral_v<T>)
    void apply_relocation(const utils::safe_buffer_accessor<std::byte> buffer, const uint64_t offset, const uint64_t delta)
    {
        const auto obj = buffer.as<T>(static_cast<size_t>(offset));
        const auto value = obj.get();
        const auto new_value = value + static_cast<T>(delta);
        obj.set(new_value);
    }

    template <typename T>
    void apply_relocations(const mapped_module& binary, const utils::safe_buffer_accessor<std::byte> buffer,
                           const PEOptionalHeader_t<T>& optional_header)
    {
        const auto delta = binary.image_base - optional_header.ImageBase;
        if (delta == 0)
        {
            return;
        }

        const auto* directory = &optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (directory->Size == 0)
        {
            return;
        }

        auto relocation_offset = directory->VirtualAddress;
        const auto relocation_end = relocation_offset + directory->Size;

        while (relocation_offset < relocation_end)
        {
            const auto relocation = buffer.as<IMAGE_BASE_RELOCATION>(relocation_offset).get();

            if (relocation.VirtualAddress <= 0 || relocation.SizeOfBlock <= sizeof(IMAGE_BASE_RELOCATION))
            {
                break;
            }

            const auto data_size = relocation.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION);
            const auto entry_count = data_size / sizeof(uint16_t);

            const auto entries = buffer.as<uint16_t>(relocation_offset + sizeof(IMAGE_BASE_RELOCATION));

            relocation_offset += relocation.SizeOfBlock;

            for (size_t i = 0; i < entry_count; ++i)
            {
                const auto entry = entries.get(i);

                const int type = entry >> 12;
                const auto offset = static_cast<uint16_t>(entry & 0xfff);
                const auto total_offset = relocation.VirtualAddress + offset;

                switch (type)
                {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;

                case IMAGE_REL_BASED_HIGHLOW:
                    apply_relocation<DWORD>(buffer, total_offset, delta);
                    break;

                case IMAGE_REL_BASED_DIR64:
                    apply_relocation<ULONGLONG>(buffer, total_offset, delta);
                    break;

                default:
                    throw std::runtime_error("Unknown relocation type: " + std::to_string(type));
                }
            }
        }
    }

    template <typename T>
    void map_sections(memory_manager& memory, mapped_module& binary, const utils::safe_buffer_accessor<const std::byte> buffer,
                      const PENTHeaders_t<T>& nt_headers, const uint64_t nt_headers_offset)
    {
        const auto first_section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
        const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(first_section_offset));

        for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
        {
            const auto section = sections.get(i);
            const auto target_ptr = binary.image_base + section.VirtualAddress;

            if (section.SizeOfRawData > 0)
            {
                const auto size_of_data = std::min(section.SizeOfRawData, section.Misc.VirtualSize);
                const auto* source_ptr = buffer.get_pointer_for_range(section.PointerToRawData, size_of_data);
                memory.write_memory(target_ptr, source_ptr, size_of_data);
            }

            auto permissions = memory_permission::none;

            if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                permissions |= memory_permission::exec;
            }

            if (section.Characteristics & IMAGE_SCN_MEM_READ)
            {
                permissions |= memory_permission::read;
            }

            if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
            {
                permissions |= memory_permission::write;
            }

            const auto size_of_section = page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize));

            memory.protect_memory(target_ptr, static_cast<size_t>(size_of_section), permissions, nullptr);

            mapped_section section_info{};
            section_info.region.start = target_ptr;
            section_info.region.length = static_cast<size_t>(size_of_section);
            section_info.region.permissions = permissions;

            for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
            {
                section_info.name.push_back(static_cast<char>(section.Name[j]));
            }

            binary.sections.push_back(std::move(section_info));
        }
    }
}

template <typename T>
mapped_module map_module_from_data(memory_manager& memory, const std::span<const std::byte> data, std::filesystem::path file, bool relocate)
{
    mapped_module binary{};
    binary.path = std::move(file);
    binary.name = binary.path.filename().string();

    utils::safe_buffer_accessor buffer{data};

    const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
    const auto nt_headers_offset = dos_header.e_lfanew;

    const auto nt_headers = buffer.as<PENTHeaders_t<T>>(nt_headers_offset).get();
    const auto& optional_header = nt_headers.OptionalHeader;

    if (nt_headers.FileHeader.Machine != PEMachineType::I386 && nt_headers.FileHeader.Machine != PEMachineType::AMD64)
    {
        throw std::runtime_error("Unsupported architecture!");
    }

    binary.image_base = optional_header.ImageBase;
    binary.image_base_file = optional_header.ImageBase;
    binary.size_of_image = page_align_up(optional_header.SizeOfImage); // TODO: Sanitize

    // Store PE header fields
    binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
    binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
    binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
    binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
    binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;

    if (!memory.allocate_memory(binary.image_base, static_cast<size_t>(binary.size_of_image), memory_permission::all))
    {
        // Check if this is a 32-bit module (WOW64)
        const bool is_32bit = (nt_headers.FileHeader.Machine == PEMachineType::I386);

        if (is_32bit)
        {
            // Use 32-bit allocation for WOW64 modules
            binary.image_base =
                memory.find_free_allocation_base(static_cast<size_t>(binary.size_of_image), DEFAULT_ALLOCATION_ADDRESS_32BIT);
        }
        else
        {
            // Use 64-bit allocation for native modules
            binary.image_base =
                memory.find_free_allocation_base(static_cast<size_t>(binary.size_of_image), DEFAULT_ALLOCATION_ADDRESS_64BIT);
        }

        const auto is_dll = nt_headers.FileHeader.Characteristics & IMAGE_FILE_DLL;
        const auto has_dynamic_base = optional_header.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
        const auto is_relocatable = is_dll || has_dynamic_base;

        if (!is_relocatable ||
            !memory.allocate_memory(binary.image_base, static_cast<size_t>(binary.size_of_image), memory_permission::all))
        {
            throw std::runtime_error("Memory range not allocatable");
        }
    }

    // TODO: Make sure to match kernel allocation patterns to attain correct initial permissions!
    memory.protect_memory(binary.image_base, static_cast<size_t>(binary.size_of_image), memory_permission::read);

    binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

    const auto* header_buffer = buffer.get_pointer_for_range(0, optional_header.SizeOfHeaders);
    memory.write_memory(binary.image_base, header_buffer, optional_header.SizeOfHeaders);

    map_sections(memory, binary, buffer, nt_headers, nt_headers_offset);

    auto mapped_memory = read_mapped_memory<T>(memory, binary);
    utils::safe_buffer_accessor<std::byte> mapped_buffer{mapped_memory};

    if (relocate)
    {
        apply_relocations(binary, mapped_buffer, optional_header);
    }

    collect_exports(binary, mapped_buffer, optional_header);
    collect_imports(binary, mapped_buffer, optional_header);

    memory.write_memory(binary.image_base, mapped_memory.data(), mapped_memory.size());

    return binary;
}

template <typename T>
mapped_module map_module_from_file(memory_manager& memory, std::filesystem::path file, bool relocate)
{
    const auto data = utils::io::read_file(file);
    if (data.empty())
    {
        throw std::runtime_error("Bad file data: " + file.string());
    }

    return map_module_from_data<T>(memory, data, std::move(file), relocate);
}

template <typename T>
mapped_module map_module_from_memory(memory_manager& memory, uint64_t base_address, uint64_t image_size, const std::string& module_name)
{
    mapped_module binary{};
    binary.name = module_name;
    binary.path = module_name;
    binary.image_base = base_address;
    binary.image_base_file = base_address;
    binary.size_of_image = image_size;

    auto mapped_memory = read_mapped_memory<T>(memory, binary);
    utils::safe_buffer_accessor<const std::byte> buffer{mapped_memory};

    try
    {
        const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
        const auto nt_headers_offset = dos_header.e_lfanew;
        const auto nt_headers = buffer.as<PENTHeaders_t<std::uint64_t>>(nt_headers_offset).get();
        const auto& optional_header = nt_headers.OptionalHeader;

        binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

        // Store PE header fields
        binary.machine = static_cast<uint16_t>(nt_headers.FileHeader.Machine);
        binary.size_of_stack_reserve = optional_header.SizeOfStackReserve;
        binary.size_of_stack_commit = optional_header.SizeOfStackCommit;
        binary.size_of_heap_reserve = optional_header.SizeOfHeapReserve;
        binary.size_of_heap_commit = optional_header.SizeOfHeapCommit;

        const auto section_offset = winpe::get_first_section_offset(nt_headers, nt_headers_offset);
        const auto sections = buffer.as<IMAGE_SECTION_HEADER>(static_cast<size_t>(section_offset));

        for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
        {
            const auto section = sections.get(i);

            mapped_section section_info{};
            section_info.region.start = binary.image_base + section.VirtualAddress;
            section_info.region.length = static_cast<size_t>(page_align_up(std::max(section.SizeOfRawData, section.Misc.VirtualSize)));

            auto permissions = memory_permission::none;
            if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            {
                permissions |= memory_permission::exec;
            }
            if (section.Characteristics & IMAGE_SCN_MEM_READ)
            {
                permissions |= memory_permission::read;
            }
            if (section.Characteristics & IMAGE_SCN_MEM_WRITE)
            {
                permissions |= memory_permission::write;
            }

            section_info.region.permissions = permissions;

            for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
            {
                section_info.name.push_back(static_cast<char>(section.Name[j]));
            }

            binary.sections.push_back(std::move(section_info));
        }

        collect_exports(binary, buffer, optional_header);
    }
    catch (const std::exception&)
    {
        // bad!
        throw std::runtime_error("Failed to map module from memory at " + std::to_string(base_address) + " with size " +
                                 std::to_string(image_size) + " for module " + module_name);
    }

    return binary;
}

bool unmap_module(memory_manager& memory, const mapped_module& mod)
{
    return memory.release_memory(mod.image_base, static_cast<size_t>(mod.size_of_image));
}

template mapped_module map_module_from_data<std::uint32_t>(memory_manager& memory, const std::span<const std::byte> data,
                                                           std::filesystem::path file, bool relocate);
template mapped_module map_module_from_data<std::uint64_t>(memory_manager& memory, const std::span<const std::byte> data,
                                                           std::filesystem::path file, bool relocate);
template mapped_module map_module_from_file<std::uint32_t>(memory_manager& memory, std::filesystem::path file, bool relocate);
template mapped_module map_module_from_file<std::uint64_t>(memory_manager& memory, std::filesystem::path file, bool relocate);

template mapped_module map_module_from_memory<std::uint32_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                             const std::string& module_name);
template mapped_module map_module_from_memory<std::uint64_t>(memory_manager& memory, uint64_t base_address, uint64_t image_size,
                                                             const std::string& module_name);
