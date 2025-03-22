#pragma once

#include <span>
#include <vector>
#include <cstdint>

namespace utils::compression
{
    namespace zlib
    {
        constexpr unsigned int ZCHUNK_SIZE = 16384u;
        std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data);
        std::vector<std::uint8_t> compress(std::span<const std::byte> data);
        std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> data);
        std::vector<std::uint8_t> decompress(std::span<const std::byte> data);
    }
};
