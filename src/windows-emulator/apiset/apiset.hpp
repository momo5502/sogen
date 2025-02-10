#pragma once

#include <vector>
#include <cstdint>
#include <filesystem>

#include "../emulator_utils.hpp"

namespace apiset
{
    enum class location : uint8_t
    {
        host,
        file,
        default_windows_10,
        default_windows_11
    };

    struct container
    {
        std::vector<uint8_t> data{};

        const API_SET_NAMESPACE& get() const
        {
            return *reinterpret_cast<const API_SET_NAMESPACE*>(data.data());
        }
    };

    container obtain(location location, const std::filesystem::path& root);
    container obtain(const std::filesystem::path& root);

    emulator_object<API_SET_NAMESPACE> clone(x64_emulator& emu, emulator_allocator& allocator,
                                             const API_SET_NAMESPACE& orig_api_set_map);

    emulator_object<API_SET_NAMESPACE> clone(x64_emulator& emu, emulator_allocator& allocator,
                                             const container& container);
}
