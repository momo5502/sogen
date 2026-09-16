#include <gtest/gtest.h>

#include <macos_memory_report.hpp>

#include "dyld_fixture.hpp"
#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <cinttypes>

namespace
{
    // Deltas rather than absolutes: a constructed emulator already maps the commpage, and pinning the
    // totals would make this test fail whenever unrelated start-up mappings change. The rule under test
    // is that a reservation adds to reserved but not to committed.
    bool host_dyld_available()
    {
        return std::filesystem::is_regular_file(MACOS_DYLD_HOST_PATH);
    }

    // Mirrors dyld_bringup_test's root: "/" does not work because dyld looks for the cache under
    // /System/Library/dyld, which is a Cryptex overlay rather than a real directory.
    std::filesystem::path write_launchable_root(const sogen::test::temp_directory& directory)
    {
        std::error_code failure{};
        std::filesystem::create_directories(directory.path() / "usr" / "lib");
        std::filesystem::create_directories(directory.path() / "System" / "Library");
        std::filesystem::create_symlink(MACOS_DYLD_HOST_PATH, directory.path() / "usr" / "lib" / "dyld", failure);
        std::filesystem::create_symlink("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld",
                                        directory.path() / "System" / "Library" / "dyld", failure);

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = MACOS_DYLD_HOST_PATH;
        executable.uses_lc_main = true;
        executable.code = {
            0xD2800000u, // mov x0, #0
            0xD2800030u, // mov x16, #1   (exit)
            0xD4001001u, // svc #0x80
        };

        macos_test::write_image(directory.path() / "bin" / "hello", macos_test::build_macho_image(executable));
        return directory.path();
    }

    // The wasm stage lives or dies on this number, so it is measured rather than assumed. Committed
    // counts only regions with backing store; reserved is far larger and does not matter, because
    // __PAGEZERO alone reserves 4 GiB it never backs.
    //
    // Measured 5,829,591,040 bytes under a real dyld launch, of which 5,824,552,834 is the shared cache
    // mapped eagerly and 4.8 MiB is everything else -- dyld, the executable, stacks, heap, commpage.
    // That decomposition is the point: the lazy pager keeps a bounded slice of the cache resident, and
    // there is nothing else of any size for it to fight with. The bound below tracks today's eager
    // mapping so a leak or a double-map still fails; it drops to the residency budget once the pager
    // lands.
    TEST(MacosMemoryReport, RealDyldLaunchStaysUnderTheWasmBudget)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"memory-report-launch"};
        const auto root = write_launchable_root(scratch);

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);
        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        emu->start(400'000'000);
        ASSERT_TRUE(emu->process.exit_status.has_value());

        const auto report = sogen::collect_macos_memory_report(emu->memory);
        printf("%s\n", sogen::format_macos_memory_report(report).c_str());

        constexpr uint64_t eager_cache_ceiling = 6ULL * 1024 * 1024 * 1024;
        EXPECT_LE(report.guest_committed_bytes, eager_cache_ceiling)
            << "a real launch commits " << report.guest_committed_bytes
            << " bytes, more than the whole shared cache -- something is mapped twice or never released";

        EXPECT_GT(report.guest_committed_bytes, 0ULL) << "a launch that commits nothing did not really run";
    }

    TEST(MacosMemoryReport, CountsOnlyBackedRegionsAsCommitted)
    {
        const auto emu = macos_test::make_emulator();
        const auto before = sogen::collect_macos_memory_report(emu->memory);

        ASSERT_TRUE(emu->memory.allocate_memory(0x200000000ULL, 0x8000, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.reserve_memory(0x210000000ULL, 0x40000));

        const auto after = sogen::collect_macos_memory_report(emu->memory);

        EXPECT_EQ(after.guest_committed_bytes - before.guest_committed_bytes, 0x8000ULL);
        EXPECT_EQ(after.guest_reserved_bytes - before.guest_reserved_bytes, 0x8000ULL + 0x40000ULL);
        EXPECT_EQ(after.guest_region_count - before.guest_region_count, 2ULL);
    }

    TEST(MacosMemoryReport, EmptyManagerReportsZero)
    {
        const auto emu = macos_test::make_emulator();

        // Bound through the base reference on purpose: passing emu->memory directly selects the copy
        // constructor, which is deleted because host_file_mapping owns its mapping.
        sogen::memory_interface& backing = emu->memory;
        sogen::macos_memory_manager empty{backing};

        const auto report = sogen::collect_macos_memory_report(empty);

        EXPECT_EQ(report.guest_committed_bytes, 0ULL);
        EXPECT_EQ(report.guest_reserved_bytes, 0ULL);
        EXPECT_EQ(report.guest_region_count, 0ULL);
    }

    TEST(MacosMemoryReport, FormatsEveryField)
    {
        sogen::macos_memory_report report{};
        report.guest_committed_bytes = 1;
        report.guest_reserved_bytes = 2;
        report.guest_region_count = 3;
        report.host_heap_bytes = 4;
        report.cache_resident_bytes = 5;

        EXPECT_EQ(sogen::format_macos_memory_report(report), "memory: committed=1 reserved=2 regions=3 host=4 cache-resident=5");
    }

    TEST(MacosMemoryReport, HostHeapIsQueryable)
    {
        EXPECT_GT(sogen::query_host_heap_bytes(), 0ULL);
    }
}
