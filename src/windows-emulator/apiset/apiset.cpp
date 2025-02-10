#include "apiset.hpp"
#include "default_apiset.hpp"

#include "../emulator_utils.hpp"

#include <x64_emulator.hpp>

#include <utils/io.hpp>
#include <utils/compression.hpp>

namespace apiset
{
    namespace
    {
        uint64_t copy_string(x64_emulator& emu, emulator_allocator& allocator, const void* base_ptr,
                             const uint64_t offset, const size_t length)
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

        ULONG copy_string_as_relative(x64_emulator& emu, emulator_allocator& allocator, const uint64_t result_base,
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

        std::vector<uint8_t> decompress_apiset(const std::vector<uint8_t>& apiset)
        {
            auto buffer = utils::compression::zlib::decompress(apiset);
            if (buffer.empty())
                throw std::runtime_error("Failed to decompress API-SET");
            return buffer;
        }

        std::vector<uint8_t> obtain_data(const location location, const std::filesystem::path& root)
        {
            switch (location)
            {
#ifdef OS_WINDOWS
            case location::host: {
                const auto apiSetMap =
                    reinterpret_cast<const API_SET_NAMESPACE*>(NtCurrentTeb64()->ProcessEnvironmentBlock->ApiSetMap);
                const auto* dataPtr = reinterpret_cast<const uint8_t*>(apiSetMap);
                return {dataPtr, dataPtr + apiSetMap->Size};
            }
#else
            case location::host:
                throw std::runtime_error("The APISET host location is not supported on this platform");
#endif
            case location::file: {
                const auto apiset = utils::io::read_file(root / "api-set.bin");
                if (apiset.empty())
                    throw std::runtime_error("Failed to read file api-set.bin");
                return decompress_apiset(apiset);
            }
            case location::default_windows_10: {
                const std::vector<uint8_t> apiset{apiset_w10, apiset_w10 + sizeof(apiset_w10)};
                return decompress_apiset(apiset);
            }
            case location::default_windows_11: {
                const std::vector<uint8_t> apiset{apiset_w11, apiset_w11 + sizeof(apiset_w11)};
                return decompress_apiset(apiset);
            }
            default:
                throw std::runtime_error("Bad API set location");
            }
        }
    }

    container obtain(const location location, const std::filesystem::path& root)
    {
        return {.data = obtain_data(location, root)};
    }

    container obtain(const std::filesystem::path& root)
    {
        auto apiset_loc = location::file;

        if (root.empty())
        {
#ifdef OS_WINDOWS
            apiset_loc = location::host;
#else
            apiset_loc = location::default_windows_11;
#endif
        }

        return obtain(apiset_loc, root);
    }

    emulator_object<API_SET_NAMESPACE> clone(x64_emulator& emu, emulator_allocator& allocator,
                                             const container& container)
    {
        return clone(emu, allocator, container.get());
    }

    emulator_object<API_SET_NAMESPACE> clone(x64_emulator& emu, emulator_allocator& allocator,
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

        const auto orig_ns_entries =
            offset_pointer<API_SET_NAMESPACE_ENTRY>(&orig_api_set_map, orig_api_set_map.EntryOffset);
        const auto orig_hash_entries =
            offset_pointer<API_SET_HASH_ENTRY>(&orig_api_set_map, orig_api_set_map.HashOffset);

        for (ULONG i = 0; i < orig_api_set_map.Count; ++i)
        {
            auto ns_entry = orig_ns_entries[i];
            const auto hash_entry = orig_hash_entries[i];

            ns_entry.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
                                                          ns_entry.NameOffset, ns_entry.NameLength);

            if (!ns_entry.ValueCount)
            {
                continue;
            }

            const auto values_obj = allocator.reserve<API_SET_VALUE_ENTRY>(ns_entry.ValueCount);
            const auto orig_values = offset_pointer<API_SET_VALUE_ENTRY>(&orig_api_set_map, ns_entry.ValueOffset);

            ns_entry.ValueOffset = static_cast<ULONG>(values_obj.value() - api_set_map_obj.value());

            for (ULONG j = 0; j < ns_entry.ValueCount; ++j)
            {
                auto value = orig_values[j];

                value.ValueOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(), &orig_api_set_map,
                                                            value.ValueOffset, value.ValueLength);

                if (value.NameLength)
                {
                    value.NameOffset = copy_string_as_relative(emu, allocator, api_set_map_obj.value(),
                                                               &orig_api_set_map, value.NameOffset, value.NameLength);
                }

                values_obj.write(value, j);
            }

            ns_entries_obj.write(ns_entry, i);
            hash_entries_obj.write(hash_entry, i);
        }

        return api_set_map_obj;
    }
}
