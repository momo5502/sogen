#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sogen
{
    constexpr uint32_t PNG_MAX_DIMENSION = 16384;

    // Emits stored (uncompressed) deflate blocks. There is no zlib, libpng or stb in the tree and the
    // wasm build has to keep working, so nothing is compressed -- a stored block is a fully conformant
    // zlib stream and every decoder accepts it. The cost is size, which for a screenshot is not a cost.
    //
    // Returns an empty vector on any rejection: zero or oversized dimensions, a stride narrower than a
    // row, or a buffer shorter than the rows it claims to hold. Never throws.
    std::vector<uint8_t> encode_png_rgba(uint32_t width, uint32_t height, uint32_t stride, std::span<const uint8_t> pixels);

    bool write_png_file(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t stride,
                        std::span<const uint8_t> pixels);

    uint32_t png_crc32(std::span<const uint8_t> data);
    uint32_t png_adler32(std::span<const uint8_t> data);
}
