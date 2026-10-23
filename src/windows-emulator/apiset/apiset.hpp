#pragma once

#include <vector>
#include <cstdint>
#include <filesystem>
#include <optional>
#include "../emulator_utils.hpp"

namespace sogen
{

    using apiset_map = std::map<std::u16string, std::u16string>;

    namespace apiset
    {
        struct container
        {
            std::vector<std::byte> data{};

            const API_SET_NAMESPACE& get() const
            {
                return *reinterpret_cast<const API_SET_NAMESPACE*>(data.data());
            }
        };

        container obtain(const std::filesystem::path& schema_dll);

        emulator_object<API_SET_NAMESPACE> clone(memory_interface& emu, emulator_allocator& allocator,
                                                 const API_SET_NAMESPACE& orig_api_set_map);

        emulator_object<API_SET_NAMESPACE> clone(memory_interface& emu, emulator_allocator& allocator, const container& container);

        apiset_map get_namespace_table(const API_SET_NAMESPACE* apiset_ns_data);
    }

} // namespace sogen
