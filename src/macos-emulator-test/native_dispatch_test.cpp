#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_native_dispatch.hpp>
#include <host_range_reader.hpp>
#include <module/dyld_cache_pager.hpp>
#include <module/dyld_shared_cache.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t target_base = 0x100004000ULL;

    std::vector<uint64_t> g_seen_args{};
    uint64_t g_call_count = 0;

    void reset_probe()
    {
        g_seen_args.clear();
        g_call_count = 0;
    }

    void probe_handler(const sogen::macos_native_call& call)
    {
        ++g_call_count;
        g_seen_args = {call.arg(0), call.arg(1), call.arg(2)};
        call.ret(0xC0FFEE);
    }

    TEST(NativeDispatch, PatchedEntryRunsTheHandlerAndReturnsToTheCaller)
    {
        reset_probe();

        const auto emu = macos_test::make_emulator();

        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD28000E0, // mov x0, #7
                                         0xD2800121, // mov x1, #9
                                         0x94000000, // bl  (displacement patched below)
                                         0xD2800030, // mov x16, #1
                                         0xD4001001, // svc #0x80
                                     });

        const auto displacement = static_cast<uint32_t>((target_base - (code_base + 8)) / 4) & 0x03FFFFFFu;
        const uint32_t bl_word = 0x94000000u | displacement;
        emu->memory.write_memory(code_base + 8, &bl_word, sizeof(bl_word));

        ASSERT_TRUE(emu->memory.allocate_memory(target_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        const uint32_t original = 0xD65F03C0; // ret
        emu->memory.write_memory(target_base, &original, sizeof(original));

        sogen::macos_native_dispatch dispatch{};
        dispatch.bind_entry(target_base, "TestRoutine", probe_handler);
        ASSERT_TRUE(sogen::patch_native_entry(*emu, target_base));

        uint32_t patched = 0;
        emu->memory.read_memory(target_base, &patched, sizeof(patched));
        EXPECT_EQ(patched, sogen::MACOS_ARM64_SVC_80);

        emu->set_native_dispatch(&dispatch);
        emu->start();

        EXPECT_EQ(g_call_count, 1u);
        ASSERT_EQ(g_seen_args.size(), 3u);
        EXPECT_EQ(g_seen_args[0], 7u);
        EXPECT_EQ(g_seen_args[1], 9u);
        // The caller exits with whatever the call left in x0, so the exit status is the handler's return
        // value. That is the assertion worth making: it proves ret() reached x0 *and* that control
        // resumed at the instruction after the bl rather than inside the patched function.
        EXPECT_EQ(emu->process.exit_status, 0xC0FFEE);
        EXPECT_TRUE(dispatch.handles(target_base));
        EXPECT_EQ(dispatch.name_of(target_base), "TestRoutine");
    }

    TEST(NativeDispatch, UnpatchedSvcStillReachesTheSyscallTable)
    {
        reset_probe();

        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD2800540, // mov x0, #42
                                         0xD2800030, // mov x16, #1
                                         0xD4001001, // svc #0x80  (exit)
                                     });

        sogen::macos_native_dispatch dispatch{};
        emu->set_native_dispatch(&dispatch);
        emu->start();

        EXPECT_EQ(g_call_count, 0u);
        EXPECT_EQ(emu->process.exit_status, 42);
    }

    TEST(NativeDispatch, BindReportsUnresolvedSymbolsWithoutFailing)
    {
        const auto emu = macos_test::make_emulator();

        sogen::macos_native_dispatch dispatch{};
        dispatch.register_routine(std::string{sogen::MACOS_SKYLIGHT_IMAGE_PATH}, "_SLSDefinitelyNotAnExport", probe_handler);

        const sogen::macos_cache_symbols symbols{};
        const auto bound = dispatch.bind(*emu, symbols);

        EXPECT_EQ(bound, 0u);
        EXPECT_EQ(dispatch.registered_count(), 1u);
        EXPECT_EQ(dispatch.bound_count(), 0u);
        ASSERT_EQ(dispatch.unbound_symbols().size(), 1u);
        EXPECT_EQ(dispatch.unbound_symbols()[0], "_SLSDefinitelyNotAnExport");
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(NativeDispatch, PatchRefusesUnmappedAndMisalignedAddresses)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_FALSE(sogen::patch_native_entry(*emu, 0x900000000ULL)) << "nothing is mapped there";

        // A symbol whose address is not instruction-aligned is a resolution that went wrong, and writing
        // a word across the boundary would corrupt the instruction before it as well as the one after.
        ASSERT_TRUE(emu->memory.allocate_memory(target_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all));
        EXPECT_FALSE(sogen::patch_native_entry(*emu, target_base + 2));

        uint32_t untouched = 0xFFFFFFFFu;
        ASSERT_TRUE(emu->memory.try_read_memory(target_base, &untouched, sizeof(untouched)));
        EXPECT_EQ(untouched, 0u) << "a refused patch writes nothing";

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::none);
    }

    TEST(NativeDispatch, PatchingAnAlreadyPatchedEntryIsIdempotent)
    {
        const auto emu = macos_test::make_emulator();
        ASSERT_TRUE(emu->memory.allocate_memory(target_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_exec));

        EXPECT_TRUE(sogen::patch_native_entry(*emu, target_base)) << "an r-x page is patchable: the write goes through protections";
        EXPECT_TRUE(sogen::patch_native_entry(*emu, target_base));

        uint32_t word = 0;
        ASSERT_TRUE(emu->memory.try_read_memory(target_base, &word, sizeof(word)));
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);
    }

    TEST(NativeDispatch, DuplicateRegistrationsAreRejected)
    {
        sogen::macos_native_dispatch dispatch{};
        dispatch.register_routine("image", "_Sym", probe_handler);
        dispatch.register_routine("image", "_Sym", probe_handler);
        EXPECT_EQ(dispatch.registered_count(), 1u);
    }

    struct lazy_cache_fixture
    {
        std::unique_ptr<sogen::macos_emulator> emu{};
        sogen::dyld_shared_cache_reader cache;
        sogen::dyld_cache_pager* pager{};
        std::string symbol{"_SLSMainConnectionID"};
        uint64_t export_address{};
    };

    // The browser's cache layout, reproduced natively: real cache contents behind the lazy pager, with no
    // host mapping, so patch_native_entry faces exactly what it faces in wasm. Skipped where no host
    // cache exists, same as the cache_symbols tests.
    void make_lazy_cache_fixture(lazy_cache_fixture& fixture)
    {
        const std::filesystem::path cache_path{MACOS_DYLD_CACHE_HOST_PATH};
        if (!std::filesystem::is_regular_file(cache_path))
        {
            GTEST_SKIP() << "no host cache";
        }

        fixture.cache = sogen::dyld_shared_cache_reader::parse(
            cache_path, [](const std::filesystem::path& p) { return sogen::open_dyld_cache_file(p); });

        const sogen::macos_cache_symbols symbols{fixture.cache};
        const auto address = symbols.find_export(sogen::MACOS_SKYLIGHT_IMAGE_PATH, fixture.symbol);
        if (!address.has_value())
        {
            GTEST_SKIP() << "SkyLight does not export " << fixture.symbol << " on this system";
        }
        fixture.export_address = *address;

        fixture.emu = macos_test::make_emulator();
        auto pager = std::make_unique<sogen::dyld_cache_pager>(fixture.emu->memory, sogen::default_host_range_reader(),
                                                               sogen::build_dyld_cache_backing_ranges(fixture.cache, 0));
        fixture.pager = pager.get();
        sogen::install_dyld_cache_pager(*fixture.emu, std::move(pager));
    }

    TEST(NativeDispatch, BindPatchesAnExportWhoseCachePageIsNotMaterialised)
    {
        lazy_cache_fixture fixture{};
        make_lazy_cache_fixture(fixture);

        ASSERT_FALSE(fixture.emu->memory.overlaps_mapped_region(fixture.export_address, sizeof(uint32_t)))
            << "the fixture only works if the export's page starts unmaterialised";

        uint32_t word = 0;

        sogen::macos_native_dispatch dispatch{};
        dispatch.register_routine(std::string{sogen::MACOS_SKYLIGHT_IMAGE_PATH}, fixture.symbol, probe_handler);

        const sogen::macos_cache_symbols symbols{fixture.cache};
        EXPECT_EQ(dispatch.bind(*fixture.emu, symbols), 1u);
        EXPECT_TRUE(dispatch.unbound_symbols().empty());
        EXPECT_GE(fixture.pager->paged_in_chunks(), 1u);

        ASSERT_TRUE(fixture.emu->memory.try_read_memory(fixture.export_address, &word, sizeof(word)));
        EXPECT_EQ(word, sogen::MACOS_ARM64_SVC_80);
    }

    // A patch that cannot be installed has to surface as an unbound routine with a named warning: bound +
    // unbound must add up to registered, or the gui event can report a silent 0 of 21 again.
    TEST(NativeDispatch, BindCountsAFailedPatchAsUnbound)
    {
        lazy_cache_fixture fixture{};
        make_lazy_cache_fixture(fixture);

        const uint64_t chunk = fixture.export_address & ~(sogen::MACOS_CACHE_CHUNK_SIZE - 1);
        ASSERT_TRUE(fixture.emu->memory.allocate_memory(chunk, sogen::MACOS_CACHE_CHUNK_SIZE, sogen::memory_permission::read_write))
            << "the collision this test depends on did not happen";

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        sogen::macos_native_dispatch dispatch{};
        dispatch.register_routine(std::string{sogen::MACOS_SKYLIGHT_IMAGE_PATH}, fixture.symbol, probe_handler);

        const sogen::macos_cache_symbols symbols{fixture.cache};
        EXPECT_EQ(dispatch.bind(*fixture.emu, symbols), 0u);
        EXPECT_EQ(dispatch.bound_count(), 0u);
        ASSERT_EQ(dispatch.unbound_symbols().size(), 1u);
        EXPECT_EQ(dispatch.unbound_symbols()[0], fixture.symbol);
        EXPECT_NE(captured.find("Failed to install the native trap"), std::string::npos)
            << "the failed patch was not named in the log: " << captured;
    }
}
