#pragma once

#include <span>
#include <vector>

namespace utils::compression
{
    namespace zlib
    {
        constexpr unsigned int ZCHUNK_SIZE = 16384u;
        std::vector<std::byte> compress(std::span<const std::byte> data);
        std::vector<std::byte> decompress(std::span<const std::byte> data);
    }
}
