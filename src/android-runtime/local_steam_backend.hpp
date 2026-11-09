#pragma once

#include <steam_backend.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sogen::android
{
    struct steam_launch
    {
        uint32_t app_id{};
        uint64_t steam_id{};
        std::string language{};
        std::vector<uint8_t> ownership_ticket{};
    };

    // Answers the guest's Steam calls from the provider built into this library, for devices without a Steam client.
    // Null when this build has no provider.
    std::unique_ptr<steam_backend> create_local_steam_backend(const steam_launch& launch);
}
