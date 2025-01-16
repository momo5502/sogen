#pragma once

#include <string>
#include <vector>
#include <cstdint>

#define CHUNK 16384u

namespace utils::compression
{
    namespace zlib
    {
        std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& data);
        std::vector<std::uint8_t> decompress(const std::vector<std::uint8_t>& data);
    }
};