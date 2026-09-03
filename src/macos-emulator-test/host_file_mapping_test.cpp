#include <gtest/gtest.h>

#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <host_file_mapping.hpp>

#include <fstream>
#include <ranges>
#include <vector>

namespace
{
    constexpr uint64_t map_base = 0x180000000ULL;

    std::filesystem::path write_temp_file(const sogen::test::temp_directory& dir, const std::string_view name,
                                          const std::vector<uint8_t>& bytes)
    {
        const auto path = dir.path() / name;
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        return path;
    }

    std::vector<uint8_t> read_whole_file(const std::filesystem::path& path)
    {
        std::ifstream stream{path, std::ios::binary};
        return std::vector<uint8_t>{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    }

    TEST(HostFileMapping, GranularityIsAPowerOfTwoPageSize)
    {
        const auto granularity = sogen::host_mapping_granularity();
        ASSERT_NE(granularity, 0u);
        EXPECT_EQ(granularity & (granularity - 1), 0u);
    }

    TEST(HostFileMapping, AGuestWriteNeverReachesTheHostFile)
    {
        const sogen::test::temp_directory dir{"hostmap"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE) * 2, 0xA5);
        content[0] = 0x11;
        const auto path = write_temp_file(dir, "backing.bin", content);

        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.map_host_file_range(map_base, content.size(), path, 0, sogen::memory_permission::read_write));
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 1u);

        uint8_t value{};
        emu->memory.read_memory(map_base, &value, sizeof(value));
        EXPECT_EQ(value, 0x11);

        constexpr uint8_t marker = 0x5C;
        emu->memory.write_memory(map_base, &marker, sizeof(marker));

        emu->memory.read_memory(map_base, &value, sizeof(value));
        EXPECT_EQ(value, marker);

        const auto on_disk = read_whole_file(path);
        ASSERT_EQ(on_disk.size(), content.size());
        EXPECT_EQ(on_disk[0], 0x11);
    }

    TEST(HostFileMapping, SurvivesAProtectionChangeFromExecuteToWrite)
    {
        const sogen::test::temp_directory dir{"hostmap-protect"};

        const std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x3C);
        const auto path = write_temp_file(dir, "text.bin", content);

        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.map_host_file_range(map_base, content.size(), path, 0,
                                                    sogen::memory_permission::read | sogen::memory_permission::exec));

        ASSERT_TRUE(emu->memory.protect_memory(map_base, content.size(), sogen::memory_permission::read_write));

        constexpr uint8_t marker = 0x77;
        emu->memory.write_memory(map_base, &marker, sizeof(marker));

        uint8_t value{};
        emu->memory.read_memory(map_base, &value, sizeof(value));
        EXPECT_EQ(value, marker);
        EXPECT_EQ(read_whole_file(path)[0], 0x3C);
    }

    TEST(HostFileMapping, RefusesAFileOffsetTheHostCannotHonour)
    {
        const sogen::test::temp_directory dir{"hostmap-offset"};

        const std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE) * 2, 0x01);
        const auto path = write_temp_file(dir, "backing.bin", content);

        const auto emu = macos_test::make_emulator();
        EXPECT_FALSE(emu->memory.map_host_file_range(map_base, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), path, 0x123,
                                                     sogen::memory_permission::read));
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 0u);
    }

    TEST(HostFileMapping, ASubcacheSizedFileMapsAtTheSharedCacheBase)
    {
        const std::filesystem::path cache{MACOS_DYLD_CACHE_HOST_PATH};
        const auto subcache = cache.parent_path() / "dyld_shared_cache_arm64e.01";
        if (!std::filesystem::is_regular_file(subcache))
        {
            GTEST_SKIP() << "no host dyld shared cache at " << subcache.string();
        }

        const auto file_size = static_cast<size_t>(std::filesystem::file_size(subcache));
        const auto mapped = file_size - (file_size % static_cast<size_t>(sogen::MACOS_PAGE_SIZE));
        ASSERT_GT(mapped, 1024ull * 1024ull * 1024ull);

        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.map_host_file_range(0x180088000ULL, mapped, subcache, 0, sogen::memory_permission::read));

        uint32_t first{};
        emu->memory.read_memory(0x180088000ULL, &first, sizeof(first));

        uint32_t last{};
        emu->memory.read_memory(0x180088000ULL + mapped - 4, &last, sizeof(last));

        EXPECT_NE(first + last, 0u);
    }

    // Mapping past the end of a file succeeds at the host level and only faults when a page is touched
    // -- as SIGBUS in the *emulator* process, not as a guest fault. The length has to be refused up
    // front or a short dylib takes the whole run down with it.
    TEST(HostFileMapping, RefusesALengthThatRunsPastTheEndOfTheFile)
    {
        const sogen::test::temp_directory dir{"hostmap-short"};

        const std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x42);
        const auto path = write_temp_file(dir, "short.bin", content);

        const auto emu = macos_test::make_emulator();
        EXPECT_FALSE(emu->memory.map_host_file_range(map_base, content.size() * 2, path, 0, sogen::memory_permission::read));
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 0u);

        EXPECT_FALSE(sogen::host_file_mapping::create(path, static_cast<uint64_t>(sogen::MACOS_PAGE_SIZE), content.size()).has_value())
            << "an offset at EOF leaves nothing to map";

        // Strictly past EOF has to be rejected before the remaining-bytes subtraction, which would
        // otherwise wrap and let any length through.
        EXPECT_FALSE(sogen::host_file_mapping::create(path, static_cast<uint64_t>(sogen::MACOS_PAGE_SIZE) * 4, content.size()).has_value());

        ASSERT_TRUE(emu->memory.map_host_file_range(map_base, content.size(), path, 0, sogen::memory_permission::read));
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 1u);
    }

    TEST(HostFileMapping, AMappingOutlivesTheGuestRangeItWasAliasedInto)
    {
        const sogen::test::temp_directory dir{"hostmap-lifetime"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x00);
        content[16] = 0xEE;
        const auto path = write_temp_file(dir, "aliased.bin", content);

        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.map_host_file_range(map_base, content.size(), path, 0, sogen::memory_permission::read));

        // unicorn flags a uc_mem_map_ptr block RAM_PREALLOC and never frees it, so the guest keeps
        // reading through this pointer for as long as the range is mapped. Dropping the host mapping
        // here would leave it aliasing freed memory.
        for (int i = 0; i < 4; ++i)
        {
            uint8_t value{};
            emu->memory.read_memory(map_base + 16, &value, sizeof(value));
            EXPECT_EQ(value, 0xEE);
        }

        EXPECT_EQ(emu->memory.get_host_mapping_count(), 1u);
    }

    TEST(HostFileMapping, GuestMmapOfAFileUsesTheZeroCopyRoute)
    {
        const sogen::test::temp_directory dir{"mmap-zero-copy"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE) * 4, 0x2B);
        content[0] = 0x42;
        const auto path = write_temp_file(dir, "mapped.bin", content);

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        const auto before = emu->memory.get_host_mapping_count();

        macos_test::write_guest_code(*emu, 0x300000000ULL, macos_test::mmap_file_sequence(guest_fd, content.size()));
        emu->start(16);

        EXPECT_EQ(emu->memory.get_host_mapping_count(), before + 1);

        const auto result = emu->emu().reg(sogen::arm64_register::x0);
        ASSERT_NE(result, 0u);

        uint8_t value{};
        emu->memory.read_memory(result, &value, sizeof(value));
        EXPECT_EQ(value, 0x42);
    }

    // The same guarantee as AGuestWriteNeverReachesTheHostFile, but reached the way a guest reaches it:
    // through sys_mmap. This is the path a sample takes, so it is the one that has to be safe.
    TEST(HostFileMapping, AGuestMmapWriteDoesNotReachTheHostFile)
    {
        const sogen::test::temp_directory dir{"mmap-cow"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x00);
        const std::string_view original{"DO-NOT-CLOBBER-ME"};
        std::ranges::copy(original, content.begin());
        const auto path = write_temp_file(dir, "victim.bin", content);

        const auto emu = macos_test::make_emulator();
        emu->file_sys = sogen::guest_file_system{dir.path()};

        sogen::guest_fd fd{};
        fd.host_path = path.string();
        fd.type = sogen::fd_type::file;
        const auto guest_fd = emu->process.fds.allocate(std::move(fd));
        ASSERT_GE(guest_fd, 0);

        auto words = macos_test::mmap_file_sequence(guest_fd, content.size());
        words[3] = macos_test::movz_x(2, 3, 0); // PROT_READ | PROT_WRITE

        macos_test::write_guest_code(*emu, 0x300000000ULL, words);
        emu->start(16);

        const auto result = emu->emu().reg(sogen::arm64_register::x0);
        ASSERT_NE(result, 0u);
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 1u) << "the write has to go through the aliased mapping to prove anything";

        const std::string_view clobber{"CLOBBERED-BY-THE"};
        emu->memory.write_memory(result, clobber.data(), clobber.size());

        std::vector<uint8_t> readback(clobber.size(), 0);
        emu->memory.read_memory(result, readback.data(), readback.size());
        EXPECT_EQ(std::string(readback.begin(), readback.end()), clobber) << "the guest must see its own write";

        const auto on_disk = read_whole_file(path);
        ASSERT_GE(on_disk.size(), original.size());
        EXPECT_EQ(std::string(on_disk.begin(), on_disk.begin() + static_cast<ptrdiff_t>(original.size())), original)
            << "a guest mmap write reached the user's real file";
    }

    // The copying route, which nothing else covers. It is what runs whenever the zero-copy one declines:
    // an offset the host cannot honour, and -- more to the point -- the whole browser build, where
    // host_file_mapping::create always returns nullopt because emscripten has no mmap. A guest must see
    // the same bytes either way, or a binary would behave differently in the page than natively.
    TEST(HostFileMapping, TheCopyingRouteShowsTheGuestTheFileContents)
    {
        const sogen::test::temp_directory dir{"mapfile-contents"};

        // The high bits of the index are folded in on purpose. A pattern that is periodic in 256 makes a
        // page-sized offset invisible -- MACOS_PAGE_SIZE * 7 is a multiple of 256, so a plain (i * 7 + 3)
        // reads identically at 0 and at one page in, and a route that ignored the offset would pass.
        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE) * 2);
        for (size_t i = 0; i < content.size(); ++i)
        {
            content[i] = static_cast<uint8_t>((i ^ (i >> 8)) & 0xFF);
        }

        const auto path = write_temp_file(dir, "copied.bin", content);
        const auto emu = macos_test::make_emulator();

        const auto length = static_cast<size_t>(sogen::MACOS_PAGE_SIZE);
        const auto offset = static_cast<uint64_t>(sogen::MACOS_PAGE_SIZE);

        ASSERT_TRUE(emu->memory.map_file(map_base, length, sogen::memory_permission::read_write, path, offset));
        EXPECT_EQ(emu->memory.get_host_mapping_count(), 0u) << "the copying route must not register an aliased mapping";

        std::vector<uint8_t> guest(64);
        ASSERT_TRUE(emu->memory.try_read_memory(map_base, guest.data(), guest.size()));

        for (size_t i = 0; i < guest.size(); ++i)
        {
            ASSERT_EQ(guest[i], content[static_cast<size_t>(offset) + i]) << "at " << i;
        }
    }

    // Same invariant as the aliased route, reached the other way. A copy cannot write back by
    // construction, which is exactly why it has to be shown to be a copy rather than assumed to be one.
    TEST(HostFileMapping, AWriteThroughTheCopyingRouteNeverReachesTheHostFile)
    {
        const sogen::test::temp_directory dir{"mapfile-cow"};

        std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x00);
        const std::string_view original{"DO-NOT-CLOBBER-ME"};
        std::ranges::copy(original, content.begin());

        const auto path = write_temp_file(dir, "victim.bin", content);
        const auto emu = macos_test::make_emulator();

        ASSERT_TRUE(
            emu->memory.map_file(map_base, static_cast<size_t>(sogen::MACOS_PAGE_SIZE), sogen::memory_permission::read_write, path, 0));

        const std::string_view clobber{"CLOBBERED-BY-THE"};
        emu->memory.write_memory(map_base, clobber.data(), clobber.size());

        const auto after = read_whole_file(path);
        ASSERT_GE(after.size(), original.size());
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(after.data()), original.size()), original);
    }

    // Deliberately not a failure. Darwin gives zeroes for the part of a mapping that lies past the end of
    // the file, and sys_mmap only reaches here with a descriptor it already resolved -- so what has to
    // hold is that the shortfall reads as zero rather than as whatever the allocation happened to hold.
    TEST(HostFileMapping, TheCopyingRouteZeroFillsPastTheEndOfTheFile)
    {
        const sogen::test::temp_directory dir{"mapfile-short"};

        const std::vector<uint8_t> content(64, 0x5A);
        const auto path = write_temp_file(dir, "small.bin", content);
        const auto emu = macos_test::make_emulator();

        const auto length = static_cast<size_t>(sogen::MACOS_PAGE_SIZE);
        ASSERT_TRUE(emu->memory.map_file(map_base, length, sogen::memory_permission::read_write, path, 0));

        std::vector<uint8_t> guest(128);
        ASSERT_TRUE(emu->memory.try_read_memory(map_base, guest.data(), guest.size()));

        for (size_t i = 0; i < content.size(); ++i)
        {
            ASSERT_EQ(guest[i], 0x5Au) << "at " << i;
        }

        for (size_t i = content.size(); i < guest.size(); ++i)
        {
            ASSERT_EQ(guest[i], 0u) << "past the end of the file, at " << i;
        }
    }

    TEST(HostFileMapping, TheCopyingRouteRefusesAZeroLengthRange)
    {
        const sogen::test::temp_directory dir{"mapfile-zero"};

        const std::vector<uint8_t> content(static_cast<size_t>(sogen::MACOS_PAGE_SIZE), 0x5A);
        const auto path = write_temp_file(dir, "small.bin", content);
        const auto emu = macos_test::make_emulator();

        EXPECT_FALSE(emu->memory.map_file(map_base, 0, sogen::memory_permission::read, path, 0));
    }
}
