#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include <host_range_reader.hpp>

#include "fixture_utils.hpp"

namespace
{
    std::filesystem::path write_sample(const sogen::test::temp_directory& directory)
    {
        const auto path = directory.path() / "sample.bin";

        std::vector<uint8_t> data(4096);
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<uint8_t>(i & 0xFF);
        }

        std::ofstream stream{path, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return path;
    }

    std::filesystem::path write_large_sample(const sogen::test::temp_directory& directory, const size_t size)
    {
        const auto path = directory.path() / "large.bin";

        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] = static_cast<uint8_t>(i & 0xFF);
        }

        std::ofstream stream{path, std::ios::binary};
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return path;
    }

    TEST(HostRangeReader, ReportsFileSize)
    {
        const sogen::test::temp_directory directory{"range-size"};
        const auto path = write_sample(directory);

        EXPECT_EQ(sogen::default_host_range_reader().file_size(path.string()), 4096ULL);
    }

    TEST(HostRangeReader, MissingFileHasZeroSize)
    {
        EXPECT_EQ(sogen::default_host_range_reader().file_size("/definitely/not/here.bin"), 0ULL);
    }

    TEST(HostRangeReader, MissingFileReadsNothing)
    {
        std::array<std::byte, 16> buffer{};
        EXPECT_EQ(sogen::default_host_range_reader().read("/definitely/not/here.bin", 0, buffer), 0ULL);
    }

    TEST(HostRangeReader, ReadsAnInteriorRange)
    {
        const sogen::test::temp_directory directory{"range-read"};
        const auto path = write_sample(directory);

        std::array<std::byte, 16> buffer{};
        const auto read = sogen::default_host_range_reader().read(path.string(), 1000, buffer);

        ASSERT_EQ(read, buffer.size());

        for (size_t i = 0; i < buffer.size(); ++i)
        {
            EXPECT_EQ(static_cast<uint8_t>(buffer[i]), static_cast<uint8_t>((1000 + i) & 0xFF));
        }
    }

    TEST(HostRangeReader, ClampsAtEndOfFile)
    {
        const sogen::test::temp_directory directory{"range-clamp"};
        const auto path = write_sample(directory);

        std::array<std::byte, 64> buffer{};
        EXPECT_EQ(sogen::default_host_range_reader().read(path.string(), 4090, buffer), 6ULL);
        EXPECT_EQ(sogen::default_host_range_reader().read(path.string(), 4096, buffer), 0ULL);
        EXPECT_EQ(sogen::default_host_range_reader().read(path.string(), 0xFFFFFFFFFFFFFFFFULL, buffer), 0ULL);
    }

    TEST(HostRangeReader, EmptyDestinationReadsNothing)
    {
        const sogen::test::temp_directory directory{"range-empty"};
        const auto path = write_sample(directory);

        EXPECT_EQ(sogen::default_host_range_reader().read(path.string(), 0, {}), 0ULL);
    }

    // The pager reads the same file thousands of times at scattered offsets, so what has to hold is that
    // a read is correct wherever it lands and whatever came before it -- not that any particular read
    // happens to work once. Descending order is deliberate: a reader that forgets to seek, or that leaves
    // the stream in a failed state after hitting the end, passes an ascending sweep and fails this.
    TEST(HostRangeReader, RepeatedReadsAtScatteredOffsetsStayCorrect)
    {
        const sogen::test::temp_directory directory{"range-repeat"};
        const auto path = write_sample(directory);
        auto& reader = sogen::default_host_range_reader();

        constexpr std::array<uint64_t, 7> offsets{4090, 0, 2048, 4095, 1, 3000, 0};

        for (const auto offset : offsets)
        {
            std::array<std::byte, 8> buffer{};
            const auto expected = std::min<uint64_t>(buffer.size(), 4096 - offset);
            const auto read = reader.read(path.string(), offset, buffer);

            ASSERT_EQ(read, expected) << "at offset " << offset;

            for (size_t i = 0; i < read; ++i)
            {
                EXPECT_EQ(static_cast<uint8_t>(buffer[i]), static_cast<uint8_t>((offset + i) & 0xFF)) << "at offset " << offset;
            }
        }
    }

    // The reader caches each file's size with its handle, which is what makes a page-in cheap. The price
    // is that a file which shrinks afterwards is read against a size that is no longer true: the read
    // comes up short and leaves eofbit set on the stream. An ifstream in a failed state ignores seekg, so
    // the *next* read returns nothing at all unless the state is cleared first -- and the pager cannot
    // tell that from a page of zeroes, which is the worst outcome available, because the guest then runs
    // on data that never existed.
    TEST(HostRangeReader, AShortReadDoesNotPoisonTheNextOne)
    {
        const sogen::test::temp_directory directory{"range-shrink"};
        constexpr size_t original_size = 1024 * 1024;
        const auto path = write_large_sample(directory, original_size);
        auto& reader = sogen::default_host_range_reader();

        std::array<std::byte, 8> primed{};
        ASSERT_EQ(reader.read(path.string(), 0, primed), 8ULL) << "the size has to be cached before the file changes";

        {
            std::ofstream truncate{path, std::ios::binary | std::ios::trunc};
            const std::vector<uint8_t> smaller(64, 0xAB);
            truncate.write(reinterpret_cast<const char*>(smaller.data()), static_cast<std::streamsize>(smaller.size()));
        }

        // Far enough from the primed read that the stream's own buffer cannot answer it, so the read
        // really does reach a file that is now much shorter than the cached size says.
        std::array<std::byte, 32> across_the_end{};
        EXPECT_EQ(reader.read(path.string(), original_size - 4096, across_the_end), 0ULL) << "the file now ends at 64";

        std::array<std::byte, 4> after{};
        ASSERT_EQ(reader.read(path.string(), 0, after), 4ULL) << "a short read left the stream unusable";
        EXPECT_EQ(static_cast<uint8_t>(after[0]), 0xABu);
    }

    // Underflow guard: offset - size wraps for an offset near the top of the range, and an unclamped
    // length taken from that would ask the stream for gigabytes. What has to hold is that the call is
    // refused and the reader is still usable afterwards.
    TEST(HostRangeReader, AWildOffsetIsRefusedAndLeavesTheReaderUsable)
    {
        const sogen::test::temp_directory directory{"range-wild"};
        const auto path = write_sample(directory);
        auto& reader = sogen::default_host_range_reader();

        std::array<std::byte, 32> buffer{};
        EXPECT_EQ(reader.read(path.string(), 0xFFFFFFFFFFFFFFFFULL, buffer), 0ULL);
        EXPECT_EQ(reader.read(path.string(), 0x8000000000000000ULL, buffer), 0ULL);

        std::array<std::byte, 4> after{};
        ASSERT_EQ(reader.read(path.string(), 16, after), 4ULL);
        EXPECT_EQ(static_cast<uint8_t>(after[0]), 16u);
    }
}
