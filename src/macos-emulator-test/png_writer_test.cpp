#include <gtest/gtest.h>

#include <png_writer.hpp>

#include "fixture_utils.hpp"

#include <cstdlib>
#include <fstream>

#include <array>
#include <cstring>
#include <vector>

namespace
{
    uint32_t read_be32(const std::span<const uint8_t> data, const size_t offset)
    {
        return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
               (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
    }

    // A minimal decoder for stored deflate, which is the only kind the encoder emits. Decoding what was
    // written is the only way to know the bytes came back, rather than that some bytes were produced.
    std::vector<uint8_t> inflate_stored(const std::span<const uint8_t> stream)
    {
        std::vector<uint8_t> out{};
        size_t offset = 2;

        while (offset + 5 <= stream.size())
        {
            const auto final_block = (stream[offset] & 1u) != 0;
            const auto length = static_cast<size_t>(stream[offset + 1]) | (static_cast<size_t>(stream[offset + 2]) << 8);
            const auto complement = static_cast<size_t>(stream[offset + 3]) | (static_cast<size_t>(stream[offset + 4]) << 8);

            if ((length ^ 0xFFFF) != complement)
            {
                return {};
            }

            offset += 5;
            if (length > stream.size() - offset)
            {
                return {};
            }

            out.insert(out.end(), stream.begin() + static_cast<ptrdiff_t>(offset),
                       stream.begin() + static_cast<ptrdiff_t>(offset + length));
            offset += length;

            if (final_block)
            {
                break;
            }
        }

        return out;
    }

    struct chunk
    {
        std::string type{};
        std::vector<uint8_t> payload{};
        bool crc_ok{};
    };

    std::vector<chunk> parse_chunks(const std::span<const uint8_t> png)
    {
        std::vector<chunk> chunks{};
        size_t offset = 8;

        while (offset + 12 <= png.size())
        {
            const auto length = read_be32(png, offset);
            if (length > png.size() - offset - 12)
            {
                break;
            }

            chunk entry{};
            entry.type.assign(reinterpret_cast<const char*>(png.data()) + offset + 4, 4);
            entry.payload.assign(png.begin() + static_cast<ptrdiff_t>(offset + 8),
                                 png.begin() + static_cast<ptrdiff_t>(offset + 8 + length));

            const std::span<const uint8_t> covered{png.data() + offset + 4, length + 4};
            entry.crc_ok = sogen::png_crc32(covered) == read_be32(png, offset + 8 + length);

            chunks.push_back(std::move(entry));
            offset += 12 + length;
        }

        return chunks;
    }

    std::vector<uint8_t> gradient(const uint32_t width, const uint32_t height, const uint32_t stride)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height, 0);

        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                auto* p = pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                p[0] = static_cast<uint8_t>(x);
                p[1] = static_cast<uint8_t>(y);
                p[2] = static_cast<uint8_t>(x ^ y);
                p[3] = 0xFF;
            }
        }

        return pixels;
    }

    TEST(PngWriter, ChecksumsMatchTheKnownVectors)
    {
        // "123456789" is the standard vector for both, so a transcription error in either table shows up
        // here rather than as a file no decoder will open.
        const std::string_view sample{"123456789"};
        const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(sample.data()), sample.size()};

        EXPECT_EQ(sogen::png_crc32(bytes), 0xCBF43926u);
        EXPECT_EQ(sogen::png_adler32(bytes), 0x091E01DEu);
        EXPECT_EQ(sogen::png_adler32({}), 1u) << "the empty stream's adler is 1, not 0";
    }

    TEST(PngWriter, ProducesAWellFormedFile)
    {
        constexpr uint32_t width = 7;
        constexpr uint32_t height = 5;
        constexpr uint32_t stride = width * 4;

        const auto pixels = gradient(width, height, stride);
        const auto png = sogen::encode_png_rgba(width, height, stride, pixels);

        ASSERT_FALSE(png.empty());

        constexpr std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        EXPECT_EQ(std::memcmp(png.data(), signature.data(), signature.size()), 0);

        const auto chunks = parse_chunks(png);
        ASSERT_EQ(chunks.size(), 3u);
        EXPECT_EQ(chunks[0].type, "IHDR");
        EXPECT_EQ(chunks[1].type, "IDAT");
        EXPECT_EQ(chunks[2].type, "IEND");

        for (const auto& entry : chunks)
        {
            EXPECT_TRUE(entry.crc_ok) << entry.type << " carries a wrong CRC, which every decoder rejects";
        }

        const auto& header = chunks[0].payload;
        ASSERT_EQ(header.size(), 13u);
        EXPECT_EQ(read_be32(header, 0), width);
        EXPECT_EQ(read_be32(header, 4), height);
        EXPECT_EQ(header[8], 8u) << "bit depth";
        EXPECT_EQ(header[9], 6u) << "truecolour with alpha";
        EXPECT_EQ(header[12], 0u) << "not interlaced";
    }

    // The point of the whole thing: the pixels come back. A file that merely parses proves nothing about
    // whether it holds the image.
    TEST(PngWriter, ThePixelsSurviveTheRoundTrip)
    {
        constexpr uint32_t width = 19;
        constexpr uint32_t height = 11;
        constexpr uint32_t stride = width * 4;

        const auto pixels = gradient(width, height, stride);
        const auto png = sogen::encode_png_rgba(width, height, stride, pixels);
        ASSERT_FALSE(png.empty());

        const auto chunks = parse_chunks(png);
        ASSERT_EQ(chunks.size(), 3u);

        const auto raw = inflate_stored(chunks[1].payload);
        ASSERT_EQ(raw.size(), static_cast<size_t>(width * 4 + 1) * height) << "one filter byte per row plus the row";

        for (uint32_t y = 0; y < height; ++y)
        {
            const auto* row = raw.data() + static_cast<size_t>(y) * (width * 4 + 1);
            ASSERT_EQ(row[0], 0u) << "filter 0 on row " << y;

            for (uint32_t x = 0; x < width; ++x)
            {
                const auto* got = row + 1 + x * 4;
                const auto* want = pixels.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
                ASSERT_EQ(std::memcmp(got, want, 4), 0) << "pixel " << x << "," << y;
            }
        }

        EXPECT_EQ(read_be32(chunks[1].payload, chunks[1].payload.size() - 4), sogen::png_adler32(raw));
    }

    // Padding must not be written. A surface's stride is usually wider than its row, and copying the gap
    // would put whatever the compositor left there into the image.
    TEST(PngWriter, StridePaddingIsNotWritten)
    {
        constexpr uint32_t width = 4;
        constexpr uint32_t height = 3;
        constexpr uint32_t stride = width * 4 + 24;

        auto pixels = gradient(width, height, stride);
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memset(pixels.data() + static_cast<size_t>(y) * stride + width * 4, 0xAB, 24);
        }

        const auto png = sogen::encode_png_rgba(width, height, stride, pixels);
        ASSERT_FALSE(png.empty());

        const auto raw = inflate_stored(parse_chunks(png)[1].payload);
        ASSERT_EQ(raw.size(), static_cast<size_t>(width * 4 + 1) * height);

        for (const auto byte : raw)
        {
            EXPECT_NE(byte, 0xABu) << "a padding byte reached the image";
        }
    }

    TEST(PngWriter, RefusesWhatItCannotEncode)
    {
        const auto pixels = gradient(4, 4, 16);

        EXPECT_TRUE(sogen::encode_png_rgba(0, 4, 16, pixels).empty()) << "zero width";
        EXPECT_TRUE(sogen::encode_png_rgba(4, 0, 16, pixels).empty()) << "zero height";
        EXPECT_TRUE(sogen::encode_png_rgba(sogen::PNG_MAX_DIMENSION + 1, 4, 16, pixels).empty()) << "oversized";
        EXPECT_TRUE(sogen::encode_png_rgba(4, 4, 8, pixels).empty()) << "a stride narrower than one row";
        EXPECT_TRUE(sogen::encode_png_rgba(4, 4, 16, std::span{pixels}.first(16)).empty()) << "a buffer short of the rows claimed";
    }

    // A row wider than a stored block's 16-bit length has to be split across blocks, and the split is
    // where an encoder that assumed one block quietly truncates the image.
    TEST(PngWriter, ImagesLargerThanOneStoredBlockAreSplitCorrectly)
    {
        constexpr uint32_t width = 300;
        constexpr uint32_t height = 80;
        constexpr uint32_t stride = width * 4;

        const auto pixels = gradient(width, height, stride);
        const auto png = sogen::encode_png_rgba(width, height, stride, pixels);
        ASSERT_FALSE(png.empty());

        const auto chunks = parse_chunks(png);
        const auto raw = inflate_stored(chunks[1].payload);

        const auto expected = static_cast<size_t>(width * 4 + 1) * height;
        ASSERT_GT(expected, 0xFFFFu) << "the fixture has to exceed one block to test anything";
        ASSERT_EQ(raw.size(), expected);

        const auto* last = raw.data() + expected - 4;
        const auto* want = pixels.data() + static_cast<size_t>(height - 1) * stride + static_cast<size_t>(width - 1) * 4;
        EXPECT_EQ(std::memcmp(last, want, 4), 0) << "the final pixel was lost at a block boundary";
    }

    // write_png_file has no coverage otherwise, and the file is the artefact that leaves the emulator.
    // SOGEN_PNG_OUT additionally writes one where asked, so a system decoder can be pointed at it: a
    // test that only parses what it wrote proves the two halves agree with each other, not that anything
    // else in the world accepts the result.
    TEST(PngWriter, WritesAFileThatParsesBackToTheSamePixels)
    {
        constexpr uint32_t width = 160;
        constexpr uint32_t height = 96;
        constexpr uint32_t stride = width * 4;

        const auto pixels = gradient(width, height, stride);

        const sogen::test::temp_directory directory{"png-write"};
        const auto path = directory.path() / "surface.png";

        ASSERT_TRUE(sogen::write_png_file(path, width, height, stride, pixels));

        std::ifstream stream{path, std::ios::binary};
        const std::vector<uint8_t> written{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};

        EXPECT_EQ(written, sogen::encode_png_rgba(width, height, stride, pixels));

        const auto chunks = parse_chunks(written);
        ASSERT_EQ(chunks.size(), 3u);
        for (const auto& entry : chunks)
        {
            EXPECT_TRUE(entry.crc_ok) << entry.type;
        }

        if (const auto* out = std::getenv("SOGEN_PNG_OUT"))
        {
            EXPECT_TRUE(sogen::write_png_file(out, width, height, stride, pixels));
        }

        EXPECT_FALSE(sogen::write_png_file(directory.path() / "nope" / "surface.png", width, height, stride, pixels))
            << "a path that cannot be opened has to be reported, not swallowed";
    }
}
