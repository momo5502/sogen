#pragma once

#include <cstdint>

namespace gdb_stub
{
    constexpr size_t CHECKSUM_SIZE = 2;

    inline uint8_t compute_checksum(const std::string_view data)
    {
        uint8_t checksum = 0;
        for (const auto c : data)
        {
            checksum += static_cast<uint8_t>(c);
        }

        return checksum;
    }
}
