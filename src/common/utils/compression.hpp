#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace utils::compression
{
    namespace zlib
    {
        constexpr unsigned int ZCHUNK_SIZE = 16384u;
        std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& data);
        std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& data);
    }
};