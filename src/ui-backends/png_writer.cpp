#include "std_include.hpp"
#include "png_writer.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <string_view>

namespace sogen
{
    namespace
    {
        constexpr std::array<uint8_t, 8> PNG_SIGNATURE{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

        // Deflate stored blocks carry a 16-bit length, so a row longer than this has to be split across
        // several. 0xFFFF is the largest a single block can describe.
        constexpr size_t MAX_STORED_BLOCK = 0xFFFF;

        void append_be32(std::vector<uint8_t>& out, const uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value >> 24));
            out.push_back(static_cast<uint8_t>(value >> 16));
            out.push_back(static_cast<uint8_t>(value >> 8));
            out.push_back(static_cast<uint8_t>(value));
        }

        void append_chunk(std::vector<uint8_t>& out, const std::string_view type, const std::span<const uint8_t> payload)
        {
            append_be32(out, static_cast<uint32_t>(payload.size()));

            const auto crc_start = out.size();
            out.insert(out.end(), type.begin(), type.end());
            out.insert(out.end(), payload.begin(), payload.end());

            const std::span<const uint8_t> covered{out.data() + crc_start, out.size() - crc_start};
            append_be32(out, png_crc32(covered));
        }

        std::vector<uint8_t> deflate_stored(const std::span<const uint8_t> data)
        {
            std::vector<uint8_t> out{};
            out.reserve(data.size() + (data.size() / MAX_STORED_BLOCK + 1) * 5 + 6);

            // zlib header: deflate, 32 KiB window, no preset dictionary, check bits chosen so the first
            // two bytes are a multiple of 31.
            out.push_back(0x78);
            out.push_back(0x01);

            size_t offset = 0;
            do
            {
                const auto count = std::min(MAX_STORED_BLOCK, data.size() - offset);
                const auto final_block = (offset + count) >= data.size();

                out.push_back(final_block ? 1 : 0);
                out.push_back(static_cast<uint8_t>(count & 0xFF));
                out.push_back(static_cast<uint8_t>(count >> 8));
                out.push_back(static_cast<uint8_t>(~count & 0xFF));
                out.push_back(static_cast<uint8_t>((~count >> 8) & 0xFF));

                out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(offset), data.begin() + static_cast<ptrdiff_t>(offset + count));
                offset += count;
            }
            // An empty input still needs one final block, or the stream has no end.
            while (offset < data.size());

            append_be32(out, png_adler32(data));
            return out;
        }
    }

    uint32_t png_crc32(const std::span<const uint8_t> data)
    {
        static const auto table = [] {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                auto value = i;
                for (int bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        for (const auto byte : data)
        {
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        }

        return crc ^ 0xFFFFFFFFu;
    }

    uint32_t png_adler32(const std::span<const uint8_t> data)
    {
        constexpr uint32_t MODULUS = 65521;

        uint32_t low = 1;
        uint32_t high = 0;

        for (const auto byte : data)
        {
            low = (low + byte) % MODULUS;
            high = (high + low) % MODULUS;
        }

        return (high << 16) | low;
    }

    std::vector<uint8_t> encode_png_rgba(const uint32_t width, const uint32_t height, const uint32_t stride,
                                         const std::span<const uint8_t> pixels)
    {
        if (width == 0 || height == 0 || width > PNG_MAX_DIMENSION || height > PNG_MAX_DIMENSION)
        {
            return {};
        }

        const auto row_bytes = static_cast<uint64_t>(width) * 4;
        if (stride < row_bytes)
        {
            return {};
        }

        const auto required = static_cast<uint64_t>(stride) * (height - 1) + row_bytes;
        if (pixels.size() < required)
        {
            return {};
        }

        // One filter byte per row, filter 0 (none). Filtering exists to help compression, and nothing
        // here compresses.
        std::vector<uint8_t> raw{};
        raw.reserve(static_cast<size_t>((row_bytes + 1) * height));

        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0);
            const auto row = pixels.subspan(static_cast<size_t>(y) * stride, static_cast<size_t>(row_bytes));
            raw.insert(raw.end(), row.begin(), row.end());
        }

        std::vector<uint8_t> out{};
        out.insert(out.end(), PNG_SIGNATURE.begin(), PNG_SIGNATURE.end());

        std::array<uint8_t, 13> header{};
        header[0] = static_cast<uint8_t>(width >> 24);
        header[1] = static_cast<uint8_t>(width >> 16);
        header[2] = static_cast<uint8_t>(width >> 8);
        header[3] = static_cast<uint8_t>(width);
        header[4] = static_cast<uint8_t>(height >> 24);
        header[5] = static_cast<uint8_t>(height >> 16);
        header[6] = static_cast<uint8_t>(height >> 8);
        header[7] = static_cast<uint8_t>(height);
        header[8] = 8;  // bit depth
        header[9] = 6;  // colour type: truecolour with alpha
        header[10] = 0; // deflate
        header[11] = 0; // adaptive filtering
        header[12] = 0; // no interlace

        append_chunk(out, "IHDR", header);
        append_chunk(out, "IDAT", deflate_stored(raw));
        append_chunk(out, "IEND", {});

        return out;
    }

    bool write_png_file(const std::filesystem::path& path, const uint32_t width, const uint32_t height, const uint32_t stride,
                        const std::span<const uint8_t> pixels)
    {
        const auto encoded = encode_png_rgba(width, height, stride, pixels);
        if (encoded.empty())
        {
            return false;
        }

        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        if (!stream)
        {
            return false;
        }

        stream.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        return stream.good();
    }
}
