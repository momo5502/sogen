#include <gtest/gtest.h>

#include <macos_memory_manager.hpp>
#include <module/macho_mapping.hpp>
#include <module/macos_module_manager.hpp>
#include <unicorn_arm64_emulator.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "fixture_utils.hpp"

namespace
{
    TEST(MachoMetadata, ReadsTheStaticArm64Fixture)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto path = sogen::test::fixture_path("macho_static_arm64");

        const auto slice = sogen::select_macho_slice(data, path);
        EXPECT_EQ(slice, 0u);

        const auto module = sogen::read_macho_module_metadata(data, path, slice, 0);

        EXPECT_EQ(module.name, "macho_static_arm64");
        EXPECT_EQ(module.cpu_type, sogen::macho::CPU_TYPE_ARM64);
        EXPECT_EQ(module.cpu_subtype, sogen::macho::CPU_SUBTYPE_ARM64_ALL);
        EXPECT_FALSE(module.is_arm64e());
        EXPECT_EQ(module.file_type, sogen::macho::MH_EXECUTE);
        EXPECT_EQ(module.flags, sogen::macho::MH_NOUNDEFS);

        EXPECT_EQ(module.preferred_base, 0x100000000ULL);
        EXPECT_EQ(module.image_base, 0x100000000ULL);
        EXPECT_EQ(module.page_zero_size, 0x100000000ULL);
        EXPECT_EQ(module.size_of_image, 0x8000ULL);

        ASSERT_TRUE(module.thread_entry.has_value());
        EXPECT_EQ(*module.thread_entry, 0x1000002d0ULL);
        EXPECT_FALSE(module.main_entry_offset.has_value());
        EXPECT_EQ(module.entry_point, 0x1000002d0ULL);

        EXPECT_TRUE(module.dylinker_path.empty());
        EXPECT_TRUE(module.dependent_libraries.empty());

        ASSERT_EQ(module.segments.size(), 2u);
        EXPECT_EQ(module.segments[0].name, "__TEXT");
        EXPECT_EQ(module.segments[0].start, 0x100000000ULL);
        EXPECT_EQ(module.segments[0].length, 0x4000u);
        EXPECT_EQ(module.segments[0].file_offset, 0u);
        EXPECT_EQ(module.segments[0].file_length, 16384u);
        EXPECT_EQ(module.segments[0].initial_permissions, sogen::memory_permission::read_exec);
        EXPECT_EQ(module.segments[1].name, "__LINKEDIT");
        EXPECT_EQ(module.segments[1].initial_permissions, sogen::memory_permission::read);

        ASSERT_EQ(module.sections.size(), 1u);
        EXPECT_EQ(module.sections[0].segment_name, "__TEXT");
        EXPECT_EQ(module.sections[0].name, "__text");

        EXPECT_TRUE(module.contains(0x1000002d0ULL));
        EXPECT_FALSE(module.contains(0x0ULL));
        EXPECT_FALSE(module.contains(0x100008000ULL));
    }

    TEST(MachoMetadata, SelectsTheArm64eSliceOfAFatBinary)
    {
        const auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");
        const auto path = sogen::test::fixture_path("macho_fat_arm64_arm64e");

        const auto slice = sogen::select_macho_slice(data, path);
        EXPECT_EQ(slice, 49152u);

        const auto module = sogen::read_macho_module_metadata(data, path, slice, 0);

        EXPECT_EQ(module.slice_offset, 49152u);
        EXPECT_EQ(module.cpu_subtype, 0x80000002u);
        EXPECT_TRUE(module.is_arm64e());
        ASSERT_TRUE(module.thread_entry.has_value());
        EXPECT_EQ(*module.thread_entry, 0x1000002d0ULL);
    }

    TEST(MachoMetadata, ReadsTheDynamicFixture)
    {
        const auto data = sogen::test::read_fixture("macho_dylink_arm64");
        const auto path = sogen::test::fixture_path("macho_dylink_arm64");

        const auto module = sogen::read_macho_module_metadata(data, path, 0, 0);

        EXPECT_EQ(module.file_type, sogen::macho::MH_EXECUTE);
        EXPECT_NE(module.flags & sogen::macho::MH_PIE, 0u);
        EXPECT_NE(module.flags & sogen::macho::MH_DYLDLINK, 0u);

        ASSERT_TRUE(module.main_entry_offset.has_value());
        EXPECT_EQ(*module.main_entry_offset, 1096u);
        EXPECT_EQ(module.stack_size, 0u);
        EXPECT_FALSE(module.thread_entry.has_value());
        EXPECT_EQ(module.entry_point, 0x100000000ULL + 1096u);

        EXPECT_EQ(module.dylinker_path, "/usr/lib/dyld");
        ASSERT_EQ(module.dependent_libraries.size(), 1u);
        EXPECT_EQ(module.dependent_libraries[0], "/usr/lib/libSystem.B.dylib");

        EXPECT_EQ(module.platform, sogen::macho::PLATFORM_MACOS);
        EXPECT_NE(module.min_os, 0u);

        ASSERT_EQ(module.segments.size(), 3u);
        EXPECT_EQ(module.segments[0].name, "__TEXT");
        EXPECT_EQ(module.segments[1].name, "__DATA_CONST");
        EXPECT_NE(module.segments[1].flags & sogen::macho::SG_READ_ONLY, 0u);
        EXPECT_EQ(module.segments[2].name, "__LINKEDIT");
    }

    TEST(MachoMetadata, RebasesEveryAddressWhenGivenAnImageBase)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto path = sogen::test::fixture_path("macho_static_arm64");

        const auto module = sogen::read_macho_module_metadata(data, path, 0, 0x200000000ULL);

        EXPECT_EQ(module.image_base, 0x200000000ULL);
        EXPECT_EQ(module.preferred_base, 0x100000000ULL);
        EXPECT_EQ(module.entry_point, 0x2000002d0ULL);
        EXPECT_EQ(module.segments[0].start, 0x200000000ULL);
        EXPECT_EQ(module.sections[0].start & ~0xfffULL, 0x200000000ULL);
        EXPECT_EQ(module.page_zero_size, 0x100000000ULL);
    }

    TEST(MachoMetadata, MapsSegmentProtectionsOntoMemoryPermissions)
    {
        using sogen::macho::VM_PROT_EXECUTE;
        using sogen::macho::VM_PROT_READ;
        using sogen::macho::VM_PROT_WRITE;

        EXPECT_EQ(sogen::macho_prot_to_permission(0), sogen::memory_permission::none);
        EXPECT_EQ(sogen::macho_prot_to_permission(VM_PROT_READ), sogen::memory_permission::read);
        EXPECT_EQ(sogen::macho_prot_to_permission(VM_PROT_READ | VM_PROT_WRITE), sogen::memory_permission::read_write);
        EXPECT_EQ(sogen::macho_prot_to_permission(VM_PROT_READ | VM_PROT_EXECUTE), sogen::memory_permission::read_exec);
        EXPECT_EQ(sogen::macho_prot_to_permission(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE), sogen::memory_permission::all);
    }

    TEST(MachoMetadata, RejectsGarbageAndUnsupportedArchitectures)
    {
        const std::vector<std::byte> garbage(256, std::byte{0x41});
        EXPECT_THROW((void)sogen::select_macho_slice(garbage, "garbage"), std::runtime_error);

        auto data = sogen::test::read_fixture("macho_static_arm64");
        data[4] = std::byte{0x07};
        data[5] = std::byte{0x00};
        data[6] = std::byte{0x00};
        data[7] = std::byte{0x01};

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "x86", 0, 0), std::runtime_error);
    }

    TEST(MachoMetadata, RejectsASegmentWhoseFileSizeExceedsItsMemorySize)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");

        constexpr size_t text_segment_offset = 32 + 72;
        constexpr size_t filesize_offset = text_segment_offset + 48;
        constexpr uint64_t absurd = 0x1000000ULL;
        std::memcpy(data.data() + filesize_offset, &absurd, sizeof(absurd));

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "truncated", 0, 0), std::runtime_error);
    }

    // The arm64 slice of the fat fixture sits at 16384 and is 16448 bytes long, while the file is 65600. A
    // __LINKEDIT at fileoff 16384 with filesize 16384 therefore escapes the slice while still landing inside
    // the buffer, on bytes belonging to the arm64e slice. Bounding against the buffer accepts it.
    TEST(MachoMetadata, RejectsASegmentThatEscapesItsSliceButNotTheFatBuffer)
    {
        auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");

        constexpr uint64_t arm64_slice_offset = 16384;
        constexpr uint64_t arm64_slice_size = 16448;
        constexpr size_t linkedit_segment_offset = 32 + 72 + 152;
        constexpr size_t filesize_offset = arm64_slice_offset + linkedit_segment_offset + 48;
        constexpr uint64_t linkedit_file_offset = 16384;
        constexpr uint64_t escaping_filesize = 16384;

        std::memcpy(data.data() + filesize_offset, &escaping_filesize, sizeof(escaping_filesize));

        ASSERT_GT(linkedit_file_offset + escaping_filesize, arm64_slice_size);
        ASSERT_LE(arm64_slice_offset + linkedit_file_offset + escaping_filesize, data.size());

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "fat", arm64_slice_offset, 0), std::runtime_error);
    }

    // A zero filesize used to short-circuit validation before fileoff was bounded, leaving
    // slice_offset + fileoff free to wrap into a plausible-looking offset.
    TEST(MachoMetadata, RejectsAFileOffsetOutsideTheSliceEvenWhenNothingIsMappedFromIt)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");

        constexpr size_t linkedit_segment_offset = 32 + 72 + 152;
        constexpr size_t fileoff_offset = linkedit_segment_offset + 40;
        constexpr size_t filesize_offset = linkedit_segment_offset + 48;
        constexpr uint64_t escaping_fileoff = std::numeric_limits<uint64_t>::max();
        constexpr uint64_t empty_filesize = 0;

        std::memcpy(data.data() + fileoff_offset, &escaping_fileoff, sizeof(escaping_fileoff));
        std::memcpy(data.data() + filesize_offset, &empty_filesize, sizeof(empty_filesize));

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "empty-file-range", 0, 0), std::runtime_error);
    }

    TEST(MachoMetadata, RejectsAnImageBaseThatIsNotPageAligned)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "unaligned", 0, 0x200000001ULL), std::runtime_error);
    }

    TEST(MachoMetadata, RejectsAnImageBaseThatSlidesASegmentPastTheAddressSpace)
    {
        const auto data = sogen::test::read_fixture("macho_static_arm64");

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "slid", 0, 0xFFFFFFFFFFFFC000ULL), std::runtime_error);
    }

    // vmaddr + vmsize alone stays below 2^64 here; only rounding the end up to a page crosses it.
    TEST(MachoMetadata, RejectsASegmentWhosePageAlignedEndWrapsAround)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");

        constexpr size_t text_segment_offset = 32 + 72;
        constexpr size_t vmsize_offset = text_segment_offset + 32;
        constexpr uint64_t vmsize = 0xFFFFFFFEFFFFF000ULL;
        std::memcpy(data.data() + vmsize_offset, &vmsize, sizeof(vmsize));

        EXPECT_THROW((void)sogen::read_macho_module_metadata(data, "wrapping", 0, 0), std::runtime_error);
    }

    TEST(MachoMetadata, MeasuresContainmentFromTheLowestSegmentNotFromTheMachHeader)
    {
        auto data = sogen::test::read_fixture("macho_static_arm64");

        constexpr size_t linkedit_segment_offset = 32 + 72 + 152;
        constexpr size_t vmaddr_offset = linkedit_segment_offset + 24;
        constexpr uint64_t below_the_header = 0xFFFFC000ULL;
        std::memcpy(data.data() + vmaddr_offset, &below_the_header, sizeof(below_the_header));

        const auto module = sogen::read_macho_module_metadata(data, "low-segment", 0, 0);

        EXPECT_EQ(module.image_base, 0x100000000ULL);
        EXPECT_EQ(module.image_start, below_the_header);
        EXPECT_EQ(module.size_of_image, 0x8000ULL);

        EXPECT_TRUE(module.contains(below_the_header));
        EXPECT_TRUE(module.contains(0x100000000ULL));
        EXPECT_FALSE(module.contains(0x100004000ULL));
    }

    struct loaded_guest
    {
        std::unique_ptr<sogen::arm64_mappable_emulator> emulator{sogen::unicorn::create_arm64_emulator()};
        sogen::macos_memory_manager memory{*emulator};
    };

    std::vector<std::byte> synthetic_macho(const uint32_t file_type, const uint64_t vmaddr, const std::string_view dylinker_path)
    {
        constexpr uint64_t image_size = 0x4000;

        std::vector<std::byte> commands{};
        const auto append = [&commands](const void* bytes, const size_t size) {
            const auto* first = static_cast<const std::byte*>(bytes);
            commands.insert(commands.end(), first, first + size);
        };

        sogen::macho::segment_command_64 text{};
        text.cmd = sogen::macho::LC_SEGMENT_64;
        text.cmdsize = sizeof(text);
        std::ranges::copy(std::string_view{"__TEXT"}, text.segname.begin());
        text.vmaddr = vmaddr;
        text.vmsize = image_size;
        text.filesize = image_size;
        text.maxprot = sogen::macho::VM_PROT_READ | sogen::macho::VM_PROT_EXECUTE;
        text.initprot = text.maxprot;
        append(&text, sizeof(text));

        sogen::macho::entry_point_command main_command{};
        main_command.cmd = sogen::macho::LC_MAIN;
        main_command.cmdsize = sizeof(main_command);
        main_command.entryoff = 0x100;
        append(&main_command, sizeof(main_command));

        uint32_t command_count = 2;

        if (!dylinker_path.empty())
        {
            constexpr auto name_offset = sizeof(sogen::macho::dylinker_command);
            const auto command_size = (name_offset + dylinker_path.size() + 1 + 7) & ~size_t{7};

            sogen::macho::dylinker_command dylinker{};
            dylinker.cmd = sogen::macho::LC_LOAD_DYLINKER;
            dylinker.cmdsize = static_cast<uint32_t>(command_size);
            dylinker.name = static_cast<uint32_t>(name_offset);
            append(&dylinker, sizeof(dylinker));
            append(dylinker_path.data(), dylinker_path.size());

            commands.resize(commands.size() + command_size - name_offset - dylinker_path.size(), std::byte{0});
            ++command_count;
        }

        sogen::macho::mach_header_64 header{};
        header.magic = sogen::macho::MH_MAGIC_64;
        header.cputype = sogen::macho::CPU_TYPE_ARM64;
        header.cpusubtype = sogen::macho::CPU_SUBTYPE_ARM64_ALL;
        header.filetype = file_type;
        header.ncmds = command_count;
        header.sizeofcmds = static_cast<uint32_t>(commands.size());
        header.flags = sogen::macho::MH_PIE;

        std::vector<std::byte> data(image_size, std::byte{0});
        std::memcpy(data.data(), &header, sizeof(header));
        std::memcpy(data.data() + sizeof(header), commands.data(), commands.size());

        return data;
    }

    std::filesystem::path write_macho(const std::filesystem::path& path, const std::vector<std::byte>& data)
    {
        std::filesystem::create_directories(path.parent_path());
        if (!sogen::utils::io::write_file(path, data))
        {
            throw std::runtime_error("Failed to write synthetic Mach-O: " + path.string());
        }

        return path;
    }

    TEST(MachoLoader, MapsTheStaticFixtureAndExecutesItsEntryPoint)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto module = sogen::map_macho_from_data(guest.memory, data, sogen::test::fixture_path("macho_static_arm64"));

        EXPECT_EQ(module.entry_point, 0x1000002d0ULL);

        uint32_t first_instruction{};
        guest.memory.read_memory(module.entry_point, &first_instruction, sizeof(first_instruction));
        EXPECT_EQ(first_instruction, 0xD2800540u);

        guest.emulator->reg(sogen::arm64_register::pc, module.entry_point);
        guest.emulator->start(2);

        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x0), 0x2au);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x1), 0x1234u);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::pc), module.entry_point + 8);
    }

    TEST(MachoLoader, MapsTheArm64eSliceOfTheFatFixtureAndExecutesIt)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_fat_arm64_arm64e");
        const auto module = sogen::map_macho_from_data(guest.memory, data, sogen::test::fixture_path("macho_fat_arm64_arm64e"));

        EXPECT_TRUE(module.is_arm64e());

        guest.emulator->reg(sogen::arm64_register::pc, module.entry_point);
        guest.emulator->start(1);

        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x0), 0x5eu) << "executed the arm64 slice instead of the arm64e one";
    }

    TEST(MachoLoader, PageZeroIsReservedButNeverBacked)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto module = sogen::map_macho_from_data(guest.memory, data, sogen::test::fixture_path("macho_static_arm64"));

        EXPECT_EQ(module.page_zero_size, 0x100000000ULL);
        EXPECT_TRUE(guest.memory.overlaps_mapped_region(0, sogen::MACOS_PAGE_SIZE));

        uint8_t byte{};
        EXPECT_FALSE(guest.emulator->try_read_memory(0, &byte, sizeof(byte)));
        EXPECT_FALSE(guest.emulator->try_read_memory(0xFFFFC000ULL, &byte, sizeof(byte)));

        const auto& regions = guest.memory.get_mapped_regions();
        const auto page_zero = regions.find(0);
        ASSERT_NE(page_zero, regions.end());
        EXPECT_FALSE(page_zero->second.backed);
        EXPECT_EQ(page_zero->second.length, 0x100000000ULL);
    }

    TEST(MachoLoader, AppliesSegmentProtectionsAfterCopyingFileData)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto module = sogen::map_macho_from_data(guest.memory, data, sogen::test::fixture_path("macho_static_arm64"));

        const auto& regions = guest.memory.get_mapped_regions();

        const auto text = regions.find(0x100000000ULL);
        ASSERT_NE(text, regions.end());
        EXPECT_EQ(text->second.permissions, sogen::memory_permission::read_exec);
        EXPECT_TRUE(text->second.backed);

        const auto linkedit = regions.find(0x100004000ULL);
        ASSERT_NE(linkedit, regions.end());
        EXPECT_EQ(linkedit->second.permissions, sogen::memory_permission::read);

        uint32_t magic{};
        guest.memory.read_memory(module.image_base, &magic, sizeof(magic));
        EXPECT_EQ(magic, sogen::macho::MH_MAGIC_64);
    }

    TEST(MachoLoader, HonoursAForcedBase)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto module = sogen::map_macho_from_data(guest.memory, data, sogen::test::fixture_path("macho_static_arm64"), 0x200000000ULL);

        EXPECT_EQ(module.image_base, 0x200000000ULL);
        EXPECT_EQ(module.entry_point, 0x2000002d0ULL);

        guest.emulator->reg(sogen::arm64_register::pc, module.entry_point);
        guest.emulator->start(1);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x0), 0x2au);
    }

    TEST(MachoLoader, RejectsAForcedBaseBeforeTouchingGuestMemory)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto path = sogen::test::fixture_path("macho_static_arm64");

        EXPECT_THROW((void)sogen::map_macho_from_data(guest.memory, data, path, 0xFFFFFFFFFFFFC000ULL), std::runtime_error);
        EXPECT_THROW((void)sogen::map_macho_from_data(guest.memory, data, path, 0x200000001ULL), std::runtime_error);

        EXPECT_TRUE(guest.memory.get_mapped_regions().empty());
    }

    TEST(MachoLoader, RefusesToMapTwiceOverTheSameRange)
    {
        loaded_guest guest{};

        const auto data = sogen::test::read_fixture("macho_static_arm64");
        const auto path = sogen::test::fixture_path("macho_static_arm64");

        (void)sogen::map_macho_from_data(guest.memory, data, path);
        EXPECT_THROW((void)sogen::map_macho_from_data(guest.memory, data, path), std::runtime_error);
    }

    TEST(MacosModuleManager, MapsAStaticExecutableAsTheOnlyModule)
    {
        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};

        modules.map_main_modules(sogen::test::fixture_path("macho_static_arm64"));

        ASSERT_NE(modules.executable, nullptr);
        EXPECT_EQ(modules.dylinker, nullptr);
        EXPECT_EQ(modules.get_modules().size(), 1u);
        EXPECT_EQ(modules.executable->entry_point, 0x1000002d0ULL);

        EXPECT_EQ(modules.find_by_address(0x1000002d0ULL), modules.executable);
        EXPECT_EQ(modules.find_by_address(0x900000000ULL), nullptr);
        EXPECT_EQ(modules.find_by_name("macho_static_arm64"), modules.executable);
        EXPECT_EQ(modules.find_by_name("nope"), nullptr);
    }

    TEST(MacosModuleManager, ResolvesGuestPathsThroughTheEmulationRoot)
    {
        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};

        EXPECT_EQ(modules.resolve_guest_path("/usr/lib/dyld"), std::filesystem::path{"/usr/lib/dyld"});

        modules.set_emulation_root("/tmp/sogen-macos-root");
        EXPECT_EQ(modules.resolve_guest_path("/usr/lib/dyld"), std::filesystem::path{"/tmp/sogen-macos-root"} / "usr" / "lib" / "dyld");
        EXPECT_EQ(modules.resolve_guest_path("usr/lib/dyld"), std::filesystem::path{"/tmp/sogen-macos-root"} / "usr" / "lib" / "dyld");

        const std::filesystem::path contained{std::filesystem::path{"/tmp/sogen-macos-root"} / "etc" / "passwd"};
        EXPECT_EQ(modules.resolve_guest_path("/../../../../etc/passwd"), contained);
        EXPECT_EQ(modules.resolve_guest_path("../../etc/passwd"), contained);
        EXPECT_EQ(modules.resolve_guest_path("usr/lib/../../etc/passwd"), contained);
        EXPECT_EQ(modules.resolve_guest_path("/usr/lib/../../../etc/passwd"), contained);
    }

    TEST(MacosModuleManager, RefusesADylinkerPathThatTraversesOutOfTheEmulationRoot)
    {
        const sogen::test::temp_directory directory{"dylinker-traversal"};

        const auto root = directory.path() / "root";
        std::filesystem::create_directories(root);

        write_macho(directory.path() / "outside" / "dyld", synthetic_macho(sogen::macho::MH_DYLINKER, 0, {}));

        const auto executable = write_macho(root / "main", synthetic_macho(sogen::macho::MH_EXECUTE, 0x100000000ULL, "/../outside/dyld"));

        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};
        modules.set_emulation_root(root);

        EXPECT_EQ(modules.resolve_guest_path("/../outside/dyld"), root / "outside" / "dyld");

        EXPECT_THROW(modules.map_main_modules(executable), std::runtime_error);
        EXPECT_EQ(modules.dylinker, nullptr);
    }

    TEST(MacosModuleManager, RefusesADylinkerPathNamingACharacterDevice)
    {
        const std::filesystem::path device{"/dev/zero"};
        if (!std::filesystem::exists(device))
        {
            GTEST_SKIP() << "no /dev/zero on this host";
        }

        const sogen::test::temp_directory directory{"dylinker-device"};

        const auto root = directory.path() / "root";
        std::filesystem::create_directories(root);

        const auto executable = write_macho(root / "main", synthetic_macho(sogen::macho::MH_EXECUTE, 0x100000000ULL, "/dev/zero"));

        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};
        modules.set_emulation_root(root);

        EXPECT_EQ(modules.resolve_guest_path("/dev/zero"), device);
        EXPECT_THROW((void)modules.map_module(device), std::runtime_error);

        EXPECT_THROW(modules.map_main_modules(executable), std::runtime_error);
        EXPECT_EQ(modules.dylinker, nullptr);
    }

    TEST(MacosModuleManager, MapsTheDylinkerATraversingPathResolvesToInsideTheEmulationRoot)
    {
        const sogen::test::temp_directory directory{"dylinker-contained"};

        const auto root = directory.path() / "root";

        write_macho(directory.path() / "outside" / "dyld", synthetic_macho(sogen::macho::MH_DYLINKER, 0, {}));
        write_macho(root / "outside" / "dyld", synthetic_macho(sogen::macho::MH_DYLINKER, 0, {}));

        const auto executable =
            write_macho(root / "main", synthetic_macho(sogen::macho::MH_EXECUTE, 0x100000000ULL, "/../../../outside/dyld"));

        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};
        modules.set_emulation_root(root);

        modules.map_main_modules(executable);

        ASSERT_NE(modules.dylinker, nullptr);
        EXPECT_EQ(modules.dylinker->path, root / "outside" / "dyld");
        EXPECT_EQ(modules.dylinker->image_base, sogen::MACOS_DYLD_DEFAULT_BASE);
    }

    TEST(MacosModuleManager, ReportsAMissingDylinker)
    {
        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};

        modules.set_emulation_root("/tmp/sogen-macos-root-that-does-not-exist");

        EXPECT_THROW(modules.map_main_modules(sogen::test::fixture_path("macho_dylink_arm64")), std::runtime_error);
    }

    // map_macho_from_data has no rollback: a segment that fails half way through leaves its predecessors
    // mapped. The manager therefore propagates instead of returning nullptr, so that a caller can never
    // mistake a half-mapped address space for an empty one and retry into it.
    TEST(MacosModuleManager, PropagatesAMidMappingFailureAndRecordsNothing)
    {
        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};

        constexpr uint64_t linkedit_base = 0x100004000ULL;
        ASSERT_TRUE(guest.memory.allocate_memory(linkedit_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        EXPECT_THROW((void)modules.map_module(sogen::test::fixture_path("macho_static_arm64")), std::runtime_error);

        EXPECT_TRUE(modules.get_modules().empty());
        EXPECT_EQ(modules.executable, nullptr);
        EXPECT_EQ(modules.find_by_address(0x1000002d0ULL), nullptr);

        EXPECT_TRUE(guest.memory.overlaps_mapped_region(0x100000000ULL, sogen::MACOS_PAGE_SIZE));
    }

    TEST(MacosModuleManager, MapsTheRealHostDyldAndExecutesItsFirstInstructions)
    {
        const std::filesystem::path dyld_path{MACOS_DYLD_HOST_PATH};
        if (!std::filesystem::exists(dyld_path))
        {
            GTEST_SKIP() << "no /usr/lib/dyld on this host";
        }

        loaded_guest guest{};
        sogen::macos_module_manager modules{guest.memory};

        auto* dyld = modules.map_module(dyld_path, sogen::MACOS_DYLD_DEFAULT_BASE);
        ASSERT_NE(dyld, nullptr);

        EXPECT_EQ(dyld->file_type, sogen::macho::MH_DYLINKER);
        EXPECT_TRUE(dyld->is_arm64e());
        EXPECT_EQ(dyld->slice_offset, 1163264u);
        EXPECT_EQ(dyld->preferred_base, 0u);
        EXPECT_EQ(dyld->page_zero_size, 0u);
        EXPECT_EQ(dyld->image_base, sogen::MACOS_DYLD_DEFAULT_BASE);

        ASSERT_TRUE(dyld->thread_entry.has_value());
        EXPECT_EQ(*dyld->thread_entry, 0x49c0u);
        EXPECT_EQ(dyld->entry_point, sogen::MACOS_DYLD_DEFAULT_BASE + 0x49c0ULL);

        ASSERT_GE(dyld->segments.size(), 7u);
        EXPECT_EQ(dyld->segments[0].name, "__TEXT");
        EXPECT_EQ(dyld->segments[0].length, 0xb4000u);

        constexpr uint64_t stack_base = 0x0000000700000000ULL;
        constexpr size_t stack_size = 0x10000;
        ASSERT_TRUE(guest.memory.allocate_memory(stack_base, stack_size, sogen::memory_permission::read_write));

        constexpr uint64_t stack_pointer = stack_base + stack_size - 0x1000;

        guest.emulator->reg(sogen::arm64_register::sp, stack_pointer);
        guest.emulator->reg(sogen::arm64_register::pc, dyld->entry_point);
        guest.emulator->start(4);

        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x0), stack_pointer);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::sp), stack_pointer);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x29), 0u);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::x30), 0u);
        EXPECT_EQ(guest.emulator->reg(sogen::arm64_register::pc), dyld->entry_point + 16);
    }
}
