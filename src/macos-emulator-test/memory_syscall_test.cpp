#include <gtest/gtest.h>

#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <ranges>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t carry = 0x20000000ULL;

    constexpr uint32_t mov_x16_munmap = 0xD2800930;
    constexpr uint32_t mov_x16_mprotect = 0xD2800950;
    constexpr uint32_t mov_x16_madvise = 0xD2800970;
    constexpr uint32_t mov_x16_mmap = 0xD28018B0;
    constexpr uint32_t mov_x16_shared_region_check = 0xD28024D0;

    // sys/mman.h. Its siblings live in macos_platform.hpp's macos_mmap namespace.

    // Unicorn caches translated blocks, so a second syscall replayed at an address that already ran
    // would execute the first block again; every invocation in a test gets a site of its own.
    void run_syscall(sogen::macos_emulator& emu, const size_t site, const uint32_t mov_x16, const std::vector<uint64_t>& arguments)
    {
        macos_test::write_guest_code(emu, code_base + (site * 0x40), {mov_x16, 0xD4001001});

        for (size_t i = 0; i < arguments.size(); ++i)
        {
            emu.emu().reg(static_cast<sogen::arm64_register>(static_cast<uint32_t>(sogen::arm64_register::x0) + i), arguments[i]);
        }

        emu.start(2);
    }

    void run_mmap(sogen::macos_emulator& emu, const size_t site, const uint64_t address, const uint64_t length, const int32_t protection,
                  const int32_t flags, const int64_t fd, const uint64_t offset)
    {
        run_syscall(emu, site, mov_x16_mmap,
                    {address, length, static_cast<uint64_t>(static_cast<int64_t>(protection)),
                     static_cast<uint64_t>(static_cast<int64_t>(flags)), static_cast<uint64_t>(fd), offset});
    }

    bool failed(sogen::macos_emulator& emu)
    {
        return (emu.emu().reg(sogen::arm64_register::nzcv) & carry) == carry;
    }

    uint64_t result_of(sogen::macos_emulator& emu)
    {
        return emu.emu().reg(sogen::arm64_register::x0);
    }

    struct scratch_file
    {
        std::filesystem::path path{};

        explicit scratch_file(const std::string& content)
            : path(std::filesystem::temp_directory_path() / sogen::test::unique_temp_name("sogen-macos-map"))
        {
            std::ofstream file{this->path, std::ios::binary};
            file.write(content.data(), static_cast<std::streamsize>(content.size()));
        }

        ~scratch_file()
        {
            std::error_code error{};
            std::filesystem::remove(this->path, error);
        }

        scratch_file(const scratch_file&) = delete;
        scratch_file& operator=(const scratch_file&) = delete;

        std::string read() const
        {
            std::ifstream file{this->path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }
    };

    int open_host_file_descriptor(sogen::macos_emulator& emu, const scratch_file& file)
    {
        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::file;
        entry.host_path = file.path.string();
        entry.guest_path = "/mapped";
        entry.handle = std::fopen(entry.host_path.c_str(), "rb");
        return emu.process.fds.allocate(std::move(entry));
    }

    TEST(MacosMemorySyscalls, AnonymousMmapReturnsAUsableGuestPage)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        ASSERT_FALSE(failed(*emu));
        const auto base = result_of(*emu);
        EXPECT_NE(base, 0u);
        EXPECT_EQ(base % sogen::MACOS_PAGE_SIZE, 0u);

        constexpr uint64_t probe = 0x0123456789ABCDEFULL;
        emu->memory.write_memory(base, &probe, sizeof(probe));
        uint64_t read_back{};
        emu->memory.read_memory(base, &read_back, sizeof(read_back));
        EXPECT_EQ(read_back, probe);
    }

    TEST(MacosMemorySyscalls, AnonymousMmapNeverLandsInTheCommpageNestingRegion)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        const auto base = result_of(*emu);
        EXPECT_LT(base, sogen::MACOS_COMMPAGE_NESTING_START);
        EXPECT_GE(base, sogen::MACOS_SHARED_CACHE_END);
    }

    // The emulator hands the guest buffers of its own -- window backing stores, layer contents, the
    // CoreFoundation bridge's scratch -- and releases them on its own schedule, without ever being told
    // whether the guest still holds the pointer. Recycling one of those addresses into a guest mmap is
    // what turned a stale guest write into 0xFF1E1E1E window pixels sitting in libmalloc's per-thread
    // cache.
    TEST(MacosMemorySyscalls, AGuestMmapNeverReusesAnAddressTheEmulatorHandedOut)
    {
        const auto emu = macos_test::make_emulator();

        const auto emulator_owned =
            emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        ASSERT_NE(emulator_owned, 0u);
        ASSERT_TRUE(emu->memory.release_memory(emulator_owned, sogen::MACOS_PAGE_SIZE));

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        ASSERT_FALSE(failed(*emu));
        EXPECT_NE(result_of(*emu), emulator_owned);
    }

    // The aligned-base search walks forward with a hint, and the skip used to be inferred from whether a
    // start was named -- so a guest vm_allocate asking for alignment switched off its own protection and
    // could be handed a range the emulator had released.
    TEST(MacosMemorySyscalls, AMaskedGuestSearchStillAvoidsEmulatorRanges)
    {
        const auto emu = macos_test::make_emulator();

        const auto emulator_owned =
            emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        ASSERT_NE(emulator_owned, 0u);
        ASSERT_TRUE(emu->memory.release_memory(emulator_owned, sogen::MACOS_PAGE_SIZE));

        // The same address the emulator just gave up, offered back as a search hint.
        EXPECT_NE(emu->memory.find_free_allocation_base(sogen::MACOS_PAGE_SIZE, emulator_owned, true), emulator_owned)
            << "a hinted guest search must still skip what the emulator owns";

        EXPECT_EQ(emu->memory.find_free_allocation_base(sogen::MACOS_PAGE_SIZE, emulator_owned, false), emulator_owned)
            << "and the emulator itself must still be able to take it back";
    }

    // What bounds the set of addresses withheld from the guest: the emulator keeps reusing its own, so a
    // buffer it allocates and releases over and over costs one range rather than one per cycle.
    TEST(MacosMemorySyscalls, TheEmulatorStillReusesTheAddressesItReleased)
    {
        const auto emu = macos_test::make_emulator();

        const auto first =
            emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        ASSERT_NE(first, 0u);
        ASSERT_TRUE(emu->memory.release_memory(first, sogen::MACOS_PAGE_SIZE));

        const auto second =
            emu->memory.allocate_memory(sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write, sogen::MACOS_GUI_ARENA_BASE);
        EXPECT_EQ(second, first);
    }

    TEST(MacosMemorySyscalls, FixedMmapHonoursTheRequestedAddress)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t requested = 0x400000000ULL;
        run_mmap(*emu, 0, requested, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        EXPECT_EQ(result_of(*emu), requested);
        EXPECT_TRUE(emu->memory.get_region_info(requested).has_value());
    }

    TEST(MacosMemorySyscalls, FixedMmapIntoPageZeroFails)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, sogen::MACOS_PAGE_SIZE, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 12u) << "ENOMEM";
        EXPECT_FALSE(emu->memory.get_region_info(sogen::MACOS_PAGE_SIZE).has_value());
    }

    TEST(MacosMemorySyscalls, FixedMmapAtAnUnalignedAddressReturnsThePageBase)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t requested = 0x400000000ULL + 0x100;
        run_mmap(*emu, 0, requested, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0x400000000ULL);
    }

    // MAP_FIXED replaces what is already mapped rather than failing on it. dyld depends on exactly
    // that: it reserves an image's whole span with vm_allocate and then maps each segment into that
    // span, so refusing the overlap makes every dylib outside the shared cache unloadable --
    // libobjc-trampolines.dylib among them, which aborts any Swift or AppKit process.
    TEST(MacosMemorySyscalls, FixedMmapReplacesAnExistingRegion)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE * 2, sogen::memory_permission::read_write));

        run_mmap(*emu, 0, region, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        ASSERT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), region);

        const auto replaced = emu->memory.get_region_info(region);
        ASSERT_TRUE(replaced.has_value());
        EXPECT_EQ(replaced->permissions, sogen::memory_permission::read) << "the new mapping's protection, not the old one's";

        // Only the pages the request named give way; the rest of what was there is untouched.
        const auto untouched = emu->memory.get_region_info(region + sogen::MACOS_PAGE_SIZE);
        ASSERT_TRUE(untouched.has_value());
        EXPECT_EQ(untouched->permissions, sogen::memory_permission::read_write);
    }

    // A range sogen reserves for itself is not the guest's to replace, however fixed the request.
    TEST(MacosMemorySyscalls, FixedMmapOverAReservedRangeStillFails)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 12u) << "ENOMEM";
    }

    TEST(MacosMemorySyscalls, MmapTranslatesTheProtectionBits)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t requested = 0x400000000ULL;
        run_mmap(*emu, 0, requested, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_EXEC,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        ASSERT_FALSE(failed(*emu));
        const auto info = emu->memory.get_region_info(requested);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_exec);
    }

    TEST(MacosMemorySyscalls, MmapWithProtNoneLeavesTheRegionInaccessible)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t requested = 0x400000000ULL;
        run_mmap(*emu, 0, requested, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_NONE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON | sogen::macos_mmap::MACOS_MAP_FIXED, -1, 0);

        ASSERT_FALSE(failed(*emu));
        const auto info = emu->memory.get_region_info(requested);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::none);
    }

    TEST(MacosMemorySyscalls, MmapRoundsAPartialLengthUpToAWholePage)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, 1, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        ASSERT_FALSE(failed(*emu));
        const auto info = emu->memory.get_region_info(result_of(*emu));
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->length, sogen::MACOS_PAGE_SIZE);
    }

    TEST(MacosMemorySyscalls, ZeroLengthMmapIsEinval)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, 0, sogen::macos_mmap::MACOS_PROT_READ,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
    }

    TEST(MacosMemorySyscalls, AnAbsurdMmapLengthIsRefusedWithoutAllocatingIt)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, 0xFFFFFFFFFFFFULL, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE | sogen::macos_mmap::MACOS_MAP_ANON, -1, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 12u) << "ENOMEM";
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }

    TEST(MacosMemorySyscalls, MprotectAndMunmapAreObservable)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        run_syscall(*emu, 0, mov_x16_mprotect, {region, sogen::MACOS_PAGE_SIZE, static_cast<uint64_t>(sogen::macos_mmap::MACOS_PROT_READ)});

        ASSERT_FALSE(failed(*emu));
        auto info = emu->memory.get_region_info(region);
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read);

        run_syscall(*emu, 1, mov_x16_munmap, {region, sogen::MACOS_PAGE_SIZE});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_FALSE(emu->memory.get_region_info(region).has_value());
    }

    TEST(MacosMemorySyscalls, MunmapOfAnUnmappedRangeIsEinval)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, 0, mov_x16_munmap, {0x400000000ULL, sogen::MACOS_PAGE_SIZE});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
    }

    TEST(MacosMemorySyscalls, MunmapWithZeroLengthIsEinval)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        run_syscall(*emu, 0, mov_x16_munmap, {region, 0});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
        EXPECT_TRUE(emu->memory.get_region_info(region).has_value());
    }

    TEST(MacosMemorySyscalls, MprotectOfAnUnmappedRangeIsEinval)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, 0, mov_x16_mprotect,
                    {0x400000000ULL, sogen::MACOS_PAGE_SIZE, static_cast<uint64_t>(sogen::macos_mmap::MACOS_PROT_READ)});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
    }

    TEST(MacosMemorySyscalls, MadviseAlwaysSucceeds)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        run_syscall(*emu, 0, mov_x16_madvise,
                    {region, sogen::MACOS_PAGE_SIZE, static_cast<uint64_t>(sogen::macos_mmap::MACOS_MADV_FREE_REUSABLE)});

        EXPECT_EQ(result_of(*emu), 0u);
        EXPECT_FALSE(failed(*emu));
        EXPECT_TRUE(emu->memory.get_region_info(region).has_value()) << "madvise is advisory and must not release anything";
    }

    // MADV_ZERO is the one advice with an observable effect: madvise(2) says the caller may treat the
    // range as cleared, and has to be told ENOTSUP when it is not. libmalloc's calloc path uses it in
    // place of a memset for page-sized blocks, so reporting success without clearing hands the guest a
    // dirty buffer -- measured as a CFBasicHash bucket array full of stale pointers that CoreFoundation
    // released as if it had stored them.
    TEST(MacosMemorySyscalls, MadviseZeroClearsTheRange)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        constexpr size_t length = sogen::MACOS_PAGE_SIZE * 2;
        ASSERT_TRUE(emu->memory.allocate_memory(region, length, sogen::memory_permission::read_write));

        const std::vector<uint8_t> pattern(length, 0xAB);
        emu->memory.write_memory(region, pattern.data(), pattern.size());

        run_syscall(*emu, 0, mov_x16_madvise, {region, length, sogen::macos_mmap::MACOS_MADV_ZERO});

        EXPECT_FALSE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 0u);

        std::vector<uint8_t> observed(length, 0xFF);
        emu->memory.read_memory(region, observed.data(), observed.size());
        EXPECT_EQ(std::ranges::count(observed, 0), static_cast<ptrdiff_t>(length));
    }

    // A sub-page length still clears whole pages: madvise rounds the range out to page bounds before the
    // map ever sees it, and a caller that asked for less has no way to observe the difference.
    TEST(MacosMemorySyscalls, MadviseZeroClearsWholePages)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const std::vector<uint8_t> pattern(sogen::MACOS_PAGE_SIZE, 0xCD);
        emu->memory.write_memory(region, pattern.data(), pattern.size());

        run_syscall(*emu, 0, mov_x16_madvise, {region, 1, sogen::macos_mmap::MACOS_MADV_ZERO});

        ASSERT_FALSE(failed(*emu));

        std::vector<uint8_t> observed(sogen::MACOS_PAGE_SIZE, 0xFF);
        emu->memory.read_memory(region, observed.data(), observed.size());
        EXPECT_EQ(std::ranges::count(observed, 0), static_cast<ptrdiff_t>(sogen::MACOS_PAGE_SIZE));
    }

    TEST(MacosMemorySyscalls, MadviseZeroOverAnUnmappedRangeReportsEnotsup)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, 0, mov_x16_madvise, {0x400000000ULL, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_MADV_ZERO});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 45u) << "ENOTSUP is what madvise(2) says a kernel that cannot zero has to answer";
    }

    // A read-only range cannot be cleared, and answering success would be the same lie as not clearing a
    // writable one.
    TEST(MacosMemorySyscalls, MadviseZeroOverAReadOnlyRangeReportsEnotsup)
    {
        const auto emu = macos_test::make_emulator();

        constexpr uint64_t region = 0x400000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(region, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read));

        run_syscall(*emu, 0, mov_x16_madvise, {region, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_MADV_ZERO});

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 45u) << "ENOTSUP";
    }

    TEST(MacosMemorySyscalls, SharedRegionCheckFailsToForceThePrivateDyldPath)
    {
        const auto emu = macos_test::make_emulator();

        run_syscall(*emu, 0, mov_x16_shared_region_check, {0});

        EXPECT_TRUE(failed(*emu)) << "failing this call is what forces dyld onto DYLD_SHARED_REGION=private";
        EXPECT_EQ(result_of(*emu), 22u) << "EINVAL";
    }

    TEST(MacosMemorySyscalls, FileBackedMmapCopiesTheFileContents)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{"MAPPED-FILE-CONTENT"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE, fd, 0);

        ASSERT_FALSE(failed(*emu));
        const auto base = result_of(*emu);

        std::array<char, 20> content{};
        emu->memory.read_memory(base, content.data(), 19);
        EXPECT_STREQ(content.data(), "MAPPED-FILE-CONTENT");

        const auto info = emu->memory.get_region_info(base);
        ASSERT_TRUE(info.has_value());
        EXPECT_TRUE(info->is_file_backed);
        EXPECT_EQ(info->backing_path, file.path.string());
    }

    TEST(MacosMemorySyscalls, FileBackedMmapZeroFillsPastTheEndOfTheFile)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{"abcd"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ, sogen::macos_mmap::MACOS_MAP_PRIVATE, fd, 0);

        ASSERT_FALSE(failed(*emu));

        std::array<uint8_t, 8> tail{};
        emu->memory.read_memory(result_of(*emu) + 4, tail.data(), tail.size());
        EXPECT_EQ(tail, (std::array<uint8_t, 8>{}));
    }

    TEST(MacosMemorySyscalls, FileBackedMmapHonoursTheFileOffset)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{std::string(sogen::MACOS_PAGE_SIZE, 'A') + "TAIL"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ, sogen::macos_mmap::MACOS_MAP_PRIVATE, fd,
                 sogen::MACOS_PAGE_SIZE);

        ASSERT_FALSE(failed(*emu));

        std::array<char, 5> content{};
        emu->memory.read_memory(result_of(*emu), content.data(), 4);
        EXPECT_STREQ(content.data(), "TAIL");
    }

    // uc_mem_map_ptr would alias the host buffer with no copy-on-write, so a MAP_SHARED file mapping
    // built that way would let guest code rewrite the user's real macOS files. Stage 3 copies instead,
    // and this pins the copy in place.
    TEST(MacosMemorySyscalls, WritesToAFileMappingNeverReachTheHostFile)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{"ORIGINAL-CONTENT"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_WRITE,
                 sogen::macos_mmap::MACOS_MAP_SHARED, fd, 0);

        ASSERT_FALSE(failed(*emu));

        constexpr std::string_view overwrite = "CLOBBERED-BY-THE";
        emu->memory.write_memory(result_of(*emu), overwrite.data(), overwrite.size());

        EXPECT_EQ(file.read(), "ORIGINAL-CONTENT");
    }

    TEST(MacosMemorySyscalls, FileBackedMmapKeepsTheRequestedProtection)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{"read only"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ | sogen::macos_mmap::MACOS_PROT_EXEC,
                 sogen::macos_mmap::MACOS_MAP_PRIVATE, fd, 0);

        ASSERT_FALSE(failed(*emu));

        const auto info = emu->memory.get_region_info(result_of(*emu));
        ASSERT_TRUE(info.has_value());
        EXPECT_EQ(info->permissions, sogen::memory_permission::read_exec) << "the staging write must not leave the mapping writable";
    }

    TEST(MacosMemorySyscalls, FileBackedMmapWithABadDescriptorIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ, sogen::macos_mmap::MACOS_MAP_PRIVATE, 999, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 9u) << "EBADF";
    }

    TEST(MacosMemorySyscalls, FileBackedMmapOfADirectoryDescriptorIsEbadf)
    {
        const auto emu = macos_test::make_emulator();

        sogen::guest_fd entry{};
        entry.type = sogen::fd_type::directory;
        entry.host_path = std::filesystem::temp_directory_path().string();
        const auto fd = emu->process.fds.allocate(std::move(entry));

        run_mmap(*emu, 0, 0, sogen::MACOS_PAGE_SIZE, sogen::macos_mmap::MACOS_PROT_READ, sogen::macos_mmap::MACOS_MAP_PRIVATE, fd, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 9u) << "EBADF";
    }

    TEST(MacosMemorySyscalls, FileBackedMmapWithAnAbsurdLengthIsRefusedWithoutAllocatingIt)
    {
        const auto emu = macos_test::make_emulator();
        const scratch_file file{"small"};
        const auto fd = open_host_file_descriptor(*emu, file);

        run_mmap(*emu, 0, 0, 0xFFFFFFFFFFFFULL, sogen::macos_mmap::MACOS_PROT_READ, sogen::macos_mmap::MACOS_MAP_PRIVATE, fd, 0);

        EXPECT_TRUE(failed(*emu));
        EXPECT_EQ(result_of(*emu), 12u) << "ENOMEM";
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::backend_error);
    }
}
