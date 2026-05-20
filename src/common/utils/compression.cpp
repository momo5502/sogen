#include "compression.hpp"

#include <zstd.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace sogen
{

namespace utils::compression
{
    namespace zstd
    {
        std::vector<std::byte> decompress(const std::span<const std::byte> data)
        {
            const auto decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());

            if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN)
            {
                return {};
            }

            std::vector<std::byte> buffer(static_cast<size_t>(decompressed_size));

            const auto result = ZSTD_decompress(buffer.data(), buffer.size(), data.data(), data.size());

            if (ZSTD_isError(result))
            {
                return {};
            }

            return buffer;
        }

        std::vector<std::byte> compress(const std::span<const std::byte> data, const int compression_level)
        {
            const auto max_size = ZSTD_compressBound(data.size());
            std::vector<std::byte> result(max_size);

            const auto compressed_size = ZSTD_compress(result.data(), max_size, data.data(), data.size(), compression_level);

            if (ZSTD_isError(compressed_size))
            {
                return {};
            }

            result.resize(compressed_size);
            return result;
        }
    }
}
} // namespace sogen
