#include "../std_include.hpp"
#include "module_mapping.hpp"
#include <address_utils.hpp>

#include <utils/io.hpp>
#include <utils/buffer_accessor.hpp>

namespace
{
    uint64_t get_first_section_offset(const PENTHeaders_t<std::uint64_t>& nt_headers, const uint64_t nt_headers_offset)
    {
        const uint8_t* nt_headers_addr = reinterpret_cast<const uint8_t*>(&nt_headers);
        size_t optional_header_offset =
            reinterpret_cast<uintptr_t>(&(nt_headers.OptionalHeader)) - reinterpret_cast<uintptr_t>(&nt_headers);
        size_t optional_header_size = nt_headers.FileHeader.SizeOfOptionalHeader;
        const uint8_t* first_section_addr = nt_headers_addr + optional_header_offset + optional_header_size;

        const auto first_section_absolute = reinterpret_cast<uint64_t>(first_section_addr);
        const auto absolute_base = reinterpret_cast<uint64_t>(&nt_headers);
        return nt_headers_offset + (first_section_absolute - absolute_base);
    }

    std::vector<uint8_t> read_mapped_memory(const emulator& emu, const mapped_module& binary)
    {
        std::vector<uint8_t> memory{};
        memory.resize(binary.size_of_image);
        emu.read_memory(binary.image_base, memory.data(), memory.size());

        return memory;
    }

    void collect_exports(mapped_module& binary, const utils::safe_buffer_accessor<const uint8_t> buffer,
                         const PEOptionalHeader_t<std::uint64_t>& optional_header)
    {
        auto& export_directory_entry = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
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
    void apply_relocation(const utils::safe_buffer_accessor<uint8_t> buffer, const uint64_t offset,
                          const uint64_t delta)
    {
        const auto obj = buffer.as<T>(offset);
        const auto value = obj.get();
        const auto new_value = value + static_cast<T>(delta);
        obj.set(new_value);
    }

    void apply_relocations(const mapped_module& binary, const utils::safe_buffer_accessor<uint8_t> buffer,
                           const PEOptionalHeader_t<std::uint64_t>& optional_header)
    {
        const auto delta = binary.image_base - optional_header.ImageBase;
        if (delta == 0)
        {
            return;
        }

        const auto directory = &optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
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

    void map_sections(emulator& emu, mapped_module& binary, const utils::safe_buffer_accessor<const uint8_t> buffer,
                      const PENTHeaders_t<std::uint64_t>& nt_headers, const uint64_t nt_headers_offset)
    {
        const auto first_section_offset = get_first_section_offset(nt_headers, nt_headers_offset);
        const auto sections = buffer.as<IMAGE_SECTION_HEADER>(first_section_offset);

        for (size_t i = 0; i < nt_headers.FileHeader.NumberOfSections; ++i)
        {
            const auto section = sections.get(i);
            const auto target_ptr = binary.image_base + section.VirtualAddress;

            if (section.SizeOfRawData > 0)
            {
                const auto size_of_data = std::min(section.SizeOfRawData, section.Misc.VirtualSize);
                const auto* source_ptr = buffer.get_pointer_for_range(section.PointerToRawData, size_of_data);
                emu.write_memory(target_ptr, source_ptr, size_of_data);
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

            emu.protect_memory(target_ptr, size_of_section, permissions, nullptr);

            mapped_section section_info{};
            section_info.region.start = target_ptr;
            section_info.region.length = size_of_section;
            section_info.region.permissions = permissions;

            for (size_t j = 0; j < sizeof(section.Name) && section.Name[j]; ++j)
            {
                section_info.name.push_back(static_cast<char>(section.Name[j]));
            }

            binary.sections.push_back(std::move(section_info));
        }
    }
}

mapped_module map_module_from_data(emulator& emu, const std::span<const uint8_t> data, std::filesystem::path file)
{
    mapped_module binary{};
    binary.path = std::move(file);
    binary.name = binary.path.filename().string();

    utils::safe_buffer_accessor buffer{data};

    const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
    const auto nt_headers_offset = dos_header.e_lfanew;

    const auto nt_headers = buffer.as<PENTHeaders_t<std::uint64_t>>(nt_headers_offset).get();
    auto& optional_header = nt_headers.OptionalHeader;

    if (nt_headers.FileHeader.Machine != PEMachineType::AMD64)
    {
        throw std::runtime_error("Unsupported architecture!");
    }

    binary.image_base = optional_header.ImageBase;
    binary.size_of_image = page_align_up(optional_header.SizeOfImage); // TODO: Sanitize

    if (!emu.allocate_memory(binary.image_base, binary.size_of_image, memory_permission::read))
    {
        binary.image_base = emu.find_free_allocation_base(binary.size_of_image);
        const auto is_dll = nt_headers.FileHeader.Characteristics & IMAGE_FILE_DLL;
        const auto has_dynamic_base = optional_header.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
        const auto is_relocatable = is_dll || has_dynamic_base;

        if (!is_relocatable || !emu.allocate_memory(binary.image_base, binary.size_of_image, memory_permission::read))
        {
            throw std::runtime_error("Memory range not allocatable");
        }
    }

    binary.entry_point = binary.image_base + optional_header.AddressOfEntryPoint;

    const auto* header_buffer = buffer.get_pointer_for_range(0, optional_header.SizeOfHeaders);
    emu.write_memory(binary.image_base, header_buffer, optional_header.SizeOfHeaders);

    map_sections(emu, binary, buffer, nt_headers, nt_headers_offset);

    auto mapped_memory = read_mapped_memory(emu, binary);
    utils::safe_buffer_accessor<uint8_t> mapped_buffer{mapped_memory};

    apply_relocations(binary, mapped_buffer, optional_header);
    collect_exports(binary, mapped_buffer, optional_header);

    emu.write_memory(binary.image_base, mapped_memory.data(), mapped_memory.size());

    return binary;
}

mapped_module map_module_from_file(emulator& emu, std::filesystem::path file)
{
    const auto data = utils::io::read_file(file);
    if (data.empty())
    {
        throw std::runtime_error("Bad file data");
    }

    return map_module_from_data(emu, data, std::move(file));
}

bool unmap_module(emulator& emu, const mapped_module& mod)
{
    return emu.release_memory(mod.image_base, mod.size_of_image);
}
