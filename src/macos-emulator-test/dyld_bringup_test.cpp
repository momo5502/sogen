#include <gtest/gtest.h>

#include "dyld_fixture.hpp"
#include <module/dyld_cache_pager.hpp>
#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    struct bringup_trace
    {
        std::vector<std::string> names{};
        std::vector<uint64_t> ids{};
        std::vector<uint64_t> program_counters{};
    };

    bool host_dyld_available()
    {
        return std::filesystem::is_regular_file(MACOS_DYLD_HOST_PATH);
    }

    // The host dylinker cannot go in the executable slot: it is a PIE with __TEXT at vmaddr 0 and only
    // the dylinker slot slides it. A synthetic executable naming it through LC_LOAD_DYLINKER is how a
    // real process reaches it, and it is what these gates load.
    std::unique_ptr<sogen::macos_emulator> make_dyld_emulator(bringup_trace& trace, const std::filesystem::path& executable)
    {
        auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{MACOS_HOST_ROOT});

        emu->callbacks.on_syscall = [&trace, raw = emu.get()](const uint64_t id, const std::string_view name) {
            trace.ids.push_back(id);
            trace.names.emplace_back(name);
            trace.program_counters.push_back(raw->emu().read_instruction_pointer());
            return sogen::instruction_hook_continuation::run_instruction;
        };

        if (!emu->load_dyld_application(executable, {"/bin/hello"}, {}))
        {
            return nullptr;
        }

        return emu;
    }

    std::filesystem::path write_dyld_client(const sogen::test::temp_directory& directory)
    {
        macos_test::macho_image_spec executable{};
        executable.dylinker_path = MACOS_DYLD_HOST_PATH;
        executable.code = {0xD4200000u};

        const auto path = directory.path() / "hello";
        macos_test::write_image(path, macos_test::build_macho_image(executable));
        return path;
    }

    // "/" is not usable as an emulation root on this OS: dyld looks for the cache under
    // /System/Library/dyld, which is a Cryptex mount overlay rather than a real directory, so a root that
    // is a plain view of the filesystem has no cache in the place dyld looks. Linking both paths a real
    // launch consults is the smallest root that behaves like a machine.
    std::filesystem::path write_launchable_root(const sogen::test::temp_directory& directory, const std::vector<uint32_t>& code)
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
        executable.code = code;

        macos_test::write_image(directory.path() / "bin" / "hello", macos_test::build_macho_image(executable));
        return directory.path();
    }

    // The whole of stage 5 in one assertion: the host's real dylinker maps the shared cache, rebases it,
    // brings up libSystem, libobjc, libdispatch and libxpc, starts libdispatch's worker thread, hands
    // control to the executable, and the executable's own instructions run and exit.
    TEST(DyldBringUp, GateCRealDyldReachesTheExecutableWhichThenExits)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup-launch"};
        const auto root = write_launchable_root(scratch, {
                                                             0xD2800000u, // mov x0, #0
                                                             0xD2800030u, // mov x16, #1   (exit)
                                                             0xD4001001u, // svc #0x80
                                                         });

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);
        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        emu->start(400'000'000);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit);
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);

        // The executable's entry is the only code in the image, so stopping inside it is what proves
        // dyld handed over rather than dying somewhere that merely looked like success.
        EXPECT_GE(emu->emu().reg(sogen::arm64_register::pc), 0x100000000ull);
    }

    // A front-end with no console cannot otherwise tell "dyld mapped the cache" from "dyld gave up and
    // fell back to loading dylibs off disk". The two look identical right up until a library that exists
    // only inside the cache fails to load, and then the error names the library rather than the cause.
    TEST(DyldBringUp, MappingTheSharedCacheIsReported)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup-cache-report"};
        const auto root = write_launchable_root(scratch, {
                                                             0xD2800000u, // mov x0, #0
                                                             0xD2800030u, // mov x16, #1   (exit)
                                                             0xD4001001u, // svc #0x80
                                                         });

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);

        uint32_t reported_mappings = 0;
        uint64_t reported_rebased = 0;
        size_t reports = 0;

        emu->callbacks.on_shared_cache_mapped = [&](const uint32_t mappings, const uint64_t rebased) {
            ++reports;
            reported_mappings = mappings;
            reported_rebased = rebased;
        };

        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));
        emu->start(400'000'000);

        ASSERT_EQ(reports, 1u) << "reported once, when the guest asks for the mapping";
        EXPECT_GT(reported_mappings, 1u) << "a real cache is many mappings across its subcaches";
        EXPECT_GT(reported_rebased, 0u) << "and its data pages carry chained fixups to rebase";
    }

    // The browser's path, run natively against the real cache. Nothing else reaches it: a host with mmap
    // always takes the aliasing route, so without forcing it the entire pager would ship untested and
    // first run in a browser where nothing can be observed.
    //
    // This is the whole apparatus at once -- ranges built from the parsed cache, chunks materialised on
    // fault, slide info applied per chunk through backing reads, residency bounded -- judged by the only
    // thing that matters: real dyld still brings the process up and the executable still exits cleanly.
    TEST(DyldBringUp, TheCacheCanBeServedEntirelyByTheLazyPager)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup-lazy"};
        const auto root = write_launchable_root(scratch, {
                                                             0xD2800000u, // mov x0, #0
                                                             0xD2800030u, // mov x16, #1   (exit)
                                                             0xD4001001u, // svc #0x80
                                                         });

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);
        emu->force_lazy_cache_paging = true;

        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        emu->start(400'000'000);

        ASSERT_NE(emu->cache_pager, nullptr) << "the lazy path was never taken";

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit)
            << "stopped: " << emu->last_stop_detail() << " at " << emu->symbolizer.format(emu->emu().read_instruction_pointer());
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);

        // The point of the exercise: a fraction of the cache resident, not all of it.
        const auto& pager = *emu->cache_pager;
        EXPECT_GT(pager.paged_in_chunks(), 0ULL) << "nothing was ever faulted in";
        EXPECT_LE(pager.resident_bytes(), sogen::MACOS_CACHE_RESIDENCY_BUDGET_DEFAULT + sogen::MACOS_CACHE_CHUNK_SIZE);

        const auto span = pager.span_end() - pager.span_start();
        EXPECT_LT(pager.resident_bytes(), span / 4) << "the pager materialised most of the cache anyway";

        printf("lazy cache: span=%.2f GiB resident=%.1f MiB chunks=%llu evicted=%llu read=%.1f MiB\n",
               static_cast<double>(span) / (1024.0 * 1024.0 * 1024.0), static_cast<double>(pager.resident_bytes()) / (1024.0 * 1024.0),
               static_cast<unsigned long long>(pager.paged_in_chunks()), static_cast<unsigned long long>(pager.evicted_chunks()),
               static_cast<double>(pager.bytes_read()) / (1024.0 * 1024.0));
    }

    TEST(DyldBringUp, GateAMachInitIsTheFirstThingRealDyldDoes)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup"};

        bringup_trace trace{};
        const auto emu = make_dyld_emulator(trace, write_dyld_client(scratch));
        ASSERT_NE(emu, nullptr);

        emu->start(2'000'000);

        ASSERT_FALSE(trace.names.empty()) << "dyld executed no syscall: " << emu->last_stop_detail();
        EXPECT_EQ(trace.names.front(), "task_self_trap") << "first syscall was " << trace.names.front();

        // _mach_init reads _COMM_PAGE_USER_PAGE_SHIFT_64 at 0xFFFFF4025 and _COMM_PAGE_KERNEL_PAGE_SHIFT
        // at 0xFFFFF4037 off the strictly read-only commpage page. A single-page commpage dies here.
        uint8_t user_page_shift{};
        emu->memory.read_memory(sogen::MACOS_COMMPAGE_RO_BASE + 0x25, &user_page_shift, sizeof(user_page_shift));
        EXPECT_EQ(user_page_shift, 14);

        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception) << emu->last_stop_detail();
    }

    TEST(DyldBringUp, GateBRealDyldAppliesItsOwnChainedFixups)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup"};

        bringup_trace trace{};
        const auto emu = make_dyld_emulator(trace, write_dyld_client(scratch));
        ASSERT_NE(emu, nullptr);

        const auto base = emu->mod_manager.dylinker->image_base;

        // Measured on build 25G76: __DATA[0] is 0x01780000000c4000 as mapped, a
        // DYLD_CHAINED_PTR_ARM64E_USERLAND24 rebase whose target is __DATA_DIRTY,__all_image_info at
        // image offset 0xc4000. Reading it before the run is what makes the assertion after the run
        // mean "dyld did this", rather than "the loader happened to leave the right value there".
        uint64_t packed{};
        emu->memory.read_memory(base + 0xc0000, &packed, sizeof(packed));
        ASSERT_EQ(packed, 0x01780000000c4000ull) << "sogen applies no fixups, so this must still be the on-disk chain entry";

        emu->start(200'000'000);

        uint64_t rebased{};
        emu->memory.read_memory(base + 0xc0000, &rebased, sizeof(rebased));

        EXPECT_EQ(rebased, base + 0xc4000) << "dyld stopped at " << emu->symbolizer.format(emu->emu().read_instruction_pointer()) << ": "
                                           << emu->last_stop_detail();
    }

    TEST(DyldBringUp, TheDyldInCacheHandoffNeverHappensInPrivateMode)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"bringup"};

        bringup_trace trace{};
        const auto emu = make_dyld_emulator(trace, write_dyld_client(scratch));
        ASSERT_NE(emu, nullptr);

        emu->start(200'000'000);

        // In default (system-wide) mode dyld transfers control to the copy of itself inside the cache and
        // reruns mach_init, so its prologue runs twice. DYLD_SHARED_REGION=private removes that handoff.
        //
        // Counted by origin rather than by name alone: libSystem calls task_self_trap from its own
        // mach_init once initialisation gets that far, and that call is not a second dyld prologue.
        ASSERT_NE(emu->mod_manager.dylinker, nullptr);
        const auto dylinker_start = emu->mod_manager.dylinker->image_start;
        const auto dylinker_end = dylinker_start + emu->mod_manager.dylinker->size_of_image;

        size_t prologues = 0;
        for (size_t i = 0; i < trace.names.size(); ++i)
        {
            const auto pc = trace.program_counters[i];
            if (trace.names[i] == "task_self_trap" && pc >= dylinker_start && pc < dylinker_end)
            {
                ++prologues;
            }
        }

        EXPECT_LE(prologues, 1u) << "dyld ran its prologue " << prologues << " times";
    }
}
