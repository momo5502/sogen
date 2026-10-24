#include "../std_include.hpp"

#include "apiset.hpp"

#include "../emulator_utils.hpp"

#include <arch_emulator.hpp>

#include <utils/io.hpp>

namespace sogen
{

    namespace apiset
    {
        namespace
        {
            uint64_t copy_string(memory_interface& emu, emulator_allocator& allocator, const void* base_ptr, const uint64_t offset,
                                 const size_t length)
            {
                if (!length)
                {
                    return 0;
                }

                const auto length_to_allocate = length + 2;
                const auto str_obj = allocator.reserve(length_to_allocate);
                emu.write_memory(str_obj, static_cast<const uint8_t*>(base_ptr) + offset, length);

                return str_obj;
            }

            ULONG copy_string_as_relative(memory_interface& emu, emulator_allocator& allocator, const uint64_t result_base,
                                          const void* base_ptr, const uint64_t offset, const size_t length)
            {
                const auto address = copy_string(emu, allocator, base_ptr, offset, length);
                if (!address)
                {
                    return 0;
                }

                assert(address > result_base);
                return static_cast<ULONG>(address - result_base);
            }

            template <typename T>
            T read_schema_value(const std::vector<std::byte>& file, const uint64_t offset)
            {
                if (offset > file.size() || file.size() - offset < sizeof(T))
                {
                    throw std::runtime_error("Truncated apisetschema.dll");
                }

                T value{};
                memcpy(&value, file.data() + offset, sizeof(value));
                return value;
            }

            // Windows maps the .apiset section of apisetschema.dll as the process API set map.
            std::vector<std::byte> read_schema_dll(const std::filesystem::path& schema_dll)
            {
                const auto file = utils::io::read_file(schema_dll);
                if (file.empty())
                {
                    throw std::runtime_error("Failed to read apisetschema.dll");
                }

                const auto dos = read_schema_value<PEDosHeader_t>(file, 0);
                if (dos.e_magic != PEDosHeader_t::k_Magic ||
                    read_schema_value<uint32_t>(file, dos.e_lfanew) != PENTHeaders_t<uint64_t>::k_Signature)
                {
                    throw std::runtime_error("apisetschema.dll is not a PE image");
                }

                const uint64_t file_header_offset = dos.e_lfanew + sizeof(uint32_t);
                const auto file_header = read_schema_value<PEFileHeader_t>(file, file_header_offset);
                const auto first_section = file_header_offset + sizeof(PEFileHeader_t) + file_header.SizeOfOptionalHeader;

                for (uint16_t i = 0; i < file_header.NumberOfSections; ++i)
                {
                    const auto section = read_schema_value<IMAGE_SECTION_HEADER>(file, first_section + i * sizeof(IMAGE_SECTION_HEADER));
                    if (memcmp(section.Name, ".apiset", 8) != 0)
                    {
                        continue;
                    }

                    const auto api_set = read_schema_value<API_SET_NAMESPACE>(file, section.PointerToRawData);
                    if (api_set.Size < sizeof(API_SET_NAMESPACE) || api_set.Size > section.SizeOfRawData ||
                        section.PointerToRawData + static_cast<uint64_t>(api_set.Size) > file.size())
                    {
                        throw std::runtime_error("Invalid API set in apisetschema.dll");
                    }

                    const auto* begin = file.data() + section.PointerToRawData;
                    return {begin, begin + api_set.Size};
                }

                throw std::runtime_error("apisetschema.dll has no .apiset section");
            }
        }

        container obtain(const std::filesystem::path& schema_dll)
        {
            return {.data = read_schema_dll(schema_dll)};
        }

        emulator_object<API_SET_NAMESPACE> clone(memory_interface& emu, emulator_allocator& allocator, const container& container)
        {
            return clone(emu, allocator, container.get());
        }

        emulator_object<API_SET_NAMESPACE> clone(memory_interface& emu, emulator_allocator& allocator,
                                                 const API_SET_NAMESPACE& orig_api_set_map)
        {
            const auto api_set_map_obj = allocator.reserve<API_SET_NAMESPACE>();
            const auto ns_entries_obj = allocator.reserve<API_SET_NAMESPACE_ENTRY>(orig_api_set_map.Count);
            const auto hash_entries_obj = allocator.reserve<API_SET_HASH_ENTRY>(orig_api_set_map.Count);

            api_set_map_obj.access([&](API_SET_NAMESPACE& api_set) {
                api_set = orig_api_set_map;
                api_set.EntryOffset = static_cast<ULONG>(ns_entries_obj.value() - api_set_map_obj.value());
                api_set.HashOffset = static_cast<ULONG>(hash_entries_obj.value() - api_set_map_obj.value());
            });

            const auto* orig_ns_entries = offset_pointer<API_SET_NAMESPACE_ENTRY>(&orig_api_set_map, orig_api_set_map.EntryOffset);
            const auto* orig_hash_entries = offset_pointer<API_SET_HASH_ENTRY>(&orig_api_set_map, orig_api_set_map.HashOffset);

            for (ULONG i = 0; i < orig_api_set_map.Count; ++i)
            {
                auto ns_entry = orig_ns_entries[i];
                const auto hash_entry = orig_hash_entries[i];

                ns_entry.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
                                                              ns_entry.NameOffset, ns_entry.NameLength);

                if (ns_entry.ValueCount)
                {
                    const auto values_obj = allocator.reserve<API_SET_VALUE_ENTRY>(ns_entry.ValueCount);
                    const auto* orig_values = offset_pointer<API_SET_VALUE_ENTRY>(&orig_api_set_map, ns_entry.ValueOffset);

                    ns_entry.ValueOffset = static_cast<ULONG>(values_obj.value() - api_set_map_obj.value());

                    for (ULONG j = 0; j < ns_entry.ValueCount; ++j)
                    {
                        auto value = orig_values[j];

                        value.ValueOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
                                                                    value.ValueOffset, value.ValueLength);

                        if (value.NameLength)
                        {
                            value.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
                                                                       value.NameOffset, value.NameLength);
                        }

                        values_obj.write(value, j);
                    }
                }
                else
                {
                    ns_entry.ValueOffset = 0;
                }

                ns_entries_obj.write(ns_entry, i);
                hash_entries_obj.write(hash_entry, i);
            }

            api_set_map_obj.access([&](API_SET_NAMESPACE& api_set) {
                api_set.Size = static_cast<ULONG>(allocator.get_next_address() - api_set_map_obj.value());
            });

            return api_set_map_obj;
        }

        apiset_map get_namespace_table(const API_SET_NAMESPACE* apiset_ns_data)
        {
            std::map<std::u16string, std::u16string> apiset;

            for (size_t i = 0; i < apiset_ns_data->Count; i++)
            {
                const auto* entry = reinterpret_cast<const API_SET_NAMESPACE_ENTRY*>(
                    reinterpret_cast<uint64_t>(apiset_ns_data) + apiset_ns_data->EntryOffset + i * sizeof(API_SET_NAMESPACE_ENTRY));

                std::u16string name(reinterpret_cast<const char16_t*>(reinterpret_cast<uint64_t>(apiset_ns_data) + entry->NameOffset),
                                    entry->NameLength / sizeof(char16_t));

                if (!entry->ValueCount)
                {
                    continue;
                }

                const auto* value =
                    reinterpret_cast<const API_SET_VALUE_ENTRY*>(reinterpret_cast<uint64_t>(apiset_ns_data) + entry->ValueOffset +
                                                                 (entry->ValueCount - 1) * sizeof(API_SET_VALUE_ENTRY));
                std::u16string base_name(reinterpret_cast<const char16_t*>(reinterpret_cast<uint64_t>(apiset_ns_data) + value->ValueOffset),
                                         value->ValueLength / sizeof(char16_t));

                apiset[name + u".dll"] = base_name;
            }

            return apiset;
        }
    }

} // namespace sogen
