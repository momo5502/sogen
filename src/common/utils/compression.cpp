#include "compression.hpp"

#include <zstd.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

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

                const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), &ZSTD_freeCCtx};
                if (!context || ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, compression_level)))
                {
                    return {};
                }

                // Builds without multithreading reject workers and compress on the calling thread instead.
                (void)ZSTD_CCtx_setParameter(context.get(), ZSTD_c_nbWorkers, static_cast<int>(std::thread::hardware_concurrency()));

                const auto compressed_size = ZSTD_compress2(context.get(), result.data(), max_size, data.data(), data.size());
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
