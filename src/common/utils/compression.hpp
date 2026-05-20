#pragma once

#include <span>
#include <vector>

namespace sogen
{

namespace utils::compression
{
    namespace zstd
    {
        std::vector<std::byte> compress(std::span<const std::byte> data, int compression_level = 8);
        std::vector<std::byte> decompress(std::span<const std::byte> data);
    }
}
} // namespace sogen
