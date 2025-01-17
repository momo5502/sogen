#pragma once

#include <cstdint>
#include <sstream>

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

    inline std::string compute_checksum_as_string(const std::string_view data)
    {
        const auto checksum = compute_checksum(data);

        std::stringstream stream{};
        stream << std::hex << checksum;
        return stream.str();
    }
}
