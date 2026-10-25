#include <gtest/gtest.h>

#include <utils/compression.hpp>
#include <utils/serialization.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        constexpr size_t window_size = 16 * 1024 * 1024;

        std::vector<std::byte> pattern(const size_t size, const uint8_t seed)
        {
            std::vector<std::byte> data(size);
            for (size_t i = 0; i < size; ++i)
            {
                data[i] = static_cast<std::byte>(static_cast<uint8_t>(i * 31 + seed));
            }
            return data;
        }

        void write_sample(utils::buffer_serializer& serializer)
        {
            serializer.write(uint32_t{0x12345678});
            serializer.write_vector(pattern(window_size - 3, 1));
            serializer.write(uint64_t{0xCAFEBABE});
            serializer.write_chunked(2 * window_size + 5, [](const size_t offset, const std::span<std::byte> chunk) {
                const auto data = pattern(2 * window_size + 5, 2);
                std::memcpy(chunk.data(), data.data() + offset, chunk.size());
            });
            serializer.write_vector(pattern(17, 3));
        }

        void read_sample(utils::buffer_deserializer& deserializer)
        {
            EXPECT_EQ(deserializer.read<uint32_t>(), 0x12345678u);
            EXPECT_EQ(deserializer.read_vector<std::byte>(), pattern(window_size - 3, 1));
            EXPECT_EQ(deserializer.read<uint64_t>(), 0xCAFEBABEu);

            std::vector<std::byte> chunked{};
            deserializer.read_chunked(2 * window_size + 5, [&](const size_t offset, const std::span<const std::byte> chunk) {
                EXPECT_EQ(offset, chunked.size());
                chunked.insert(chunked.end(), chunk.begin(), chunk.end());
            });
            EXPECT_EQ(chunked, pattern(2 * window_size + 5, 2));

            EXPECT_EQ(deserializer.read_vector<std::byte>(), pattern(17, 3));
            EXPECT_EQ(deserializer.get_remaining_size(), 0u);
        }

        std::vector<std::byte> serialize_buffered()
        {
            utils::buffer_serializer serializer{};
            write_sample(serializer);
            return serializer.get_buffer();
        }

        utils::buffer_deserializer::source read_from(const std::vector<std::byte>& data, size_t& position)
        {
            return [&data, &position](const std::span<std::byte> out) {
                if (out.size() > data.size() - position)
                {
                    throw std::runtime_error("read past the end");
                }

                std::memcpy(out.data(), data.data() + position, out.size());
                position += out.size();
            };
        }

        std::vector<std::byte> compress_streamed(const std::span<const std::byte> data)
        {
            std::vector<std::byte> compressed{};
            utils::compression::zstd::stream_compressor compressor{
                [&](const std::span<const std::byte> chunk) { compressed.insert(compressed.end(), chunk.begin(), chunk.end()); }, 1, 0};
            compressor.write(data);
            compressor.finish();
            return compressed;
        }

        utils::compression::zstd::stream_decompressor decompressor_over(const std::vector<std::byte>& compressed, size_t& position)
        {
            return utils::compression::zstd::stream_decompressor{[&compressed, &position](const std::span<std::byte> out) {
                const auto size = std::min(out.size(), compressed.size() - position);
                std::memcpy(out.data(), compressed.data() + position, size);
                position += size;
                return size;
            }};
        }
    }

    TEST(StreamSerializationTest, SinkReceivesExactlyTheBufferedBytes)
    {
        std::vector<std::byte> streamed{};
        utils::buffer_serializer serializer{
            [&](const std::span<const std::byte> chunk) { streamed.insert(streamed.end(), chunk.begin(), chunk.end()); }};
        write_sample(serializer);
        serializer.flush();

        const auto buffered = serialize_buffered();
        EXPECT_EQ(serializer.size(), buffered.size());
        EXPECT_EQ(streamed, buffered);
    }

    TEST(StreamSerializationTest, SourceReadsEntriesAcrossWindowBoundaries)
    {
        const auto data = serialize_buffered();
        size_t position = 0;
        utils::buffer_deserializer deserializer{read_from(data, position), data.size()};
        read_sample(deserializer);
        EXPECT_EQ(position, data.size());
    }

    TEST(StreamSerializationTest, SourceRejectsReadsPastItsSize)
    {
        utils::buffer_serializer serializer{};
        serializer.write(uint32_t{1});
        const auto data = serializer.get_buffer();

        size_t position = 0;
        utils::buffer_deserializer deserializer{read_from(data, position), data.size()};
        EXPECT_EQ(deserializer.read<uint32_t>(), 1u);
        EXPECT_THROW(deserializer.read<uint32_t>(), std::runtime_error);
    }

    TEST(StreamSerializationTest, StreamedFrameDecompressesFromMemoryAndStream)
    {
        const auto data = serialize_buffered();
        const auto compressed = compress_streamed(data);

        EXPECT_EQ(utils::compression::zstd::decompress(compressed, data.size()), data);

        size_t position = 0;
        auto decompressor = decompressor_over(compressed, position);
        std::vector<std::byte> streamed(data.size());
        decompressor.read(streamed);
        decompressor.finish();
        EXPECT_EQ(streamed, data);
    }

    TEST(StreamSerializationTest, EmptyStateRoundTrips)
    {
        const auto compressed = compress_streamed({});
        EXPECT_TRUE(utils::compression::zstd::decompress(compressed, 0).empty());

        size_t position = 0;
        auto decompressor = decompressor_over(compressed, position);
        decompressor.finish();
    }

    TEST(StreamSerializationTest, TruncatedFrameIsRejected)
    {
        const auto data = pattern(window_size + 7, 4);
        auto compressed = compress_streamed(data);
        compressed.pop_back();

        EXPECT_TRUE(utils::compression::zstd::decompress(compressed, data.size()).empty());

        size_t position = 0;
        auto decompressor = decompressor_over(compressed, position);
        std::vector<std::byte> streamed(data.size());
        EXPECT_THROW(
            {
                decompressor.read(streamed);
                decompressor.finish();
            },
            std::runtime_error);
    }

    TEST(StreamSerializationTest, TrailingDataAfterTheFrameIsRejected)
    {
        const auto data = pattern(1000, 5);
        auto compressed = compress_streamed(data);
        compressed.push_back(std::byte{0});

        EXPECT_TRUE(utils::compression::zstd::decompress(compressed, data.size()).empty());

        size_t position = 0;
        auto decompressor = decompressor_over(compressed, position);
        std::vector<std::byte> streamed(data.size());
        decompressor.read(streamed);
        EXPECT_THROW(decompressor.finish(), std::runtime_error);
    }

    TEST(StreamSerializationTest, WrongDeclaredSizeIsRejected)
    {
        const auto data = pattern(1000, 6);
        const auto compressed = compress_streamed(data);

        EXPECT_TRUE(utils::compression::zstd::decompress(compressed, data.size() - 1).empty());
        EXPECT_TRUE(utils::compression::zstd::decompress(compressed, data.size() + 1).empty());
    }
}
