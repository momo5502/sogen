#include <gtest/gtest.h>

#include "dyld_fixture.hpp"
#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"
#include <string>
#include <cstdio>
#include <algorithm>
#include <array>
#include <ranges>

namespace
{
    TEST(DyldHandoff, LoadingAnImageWithADylinkerEntersTheDylinkerNotTheExecutable)
    {
        const sogen::test::temp_directory root{"handoff"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = {0xD4200000u};
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());

        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        ASSERT_NE(emu->mod_manager.dylinker, nullptr);
        ASSERT_NE(emu->mod_manager.executable, nullptr);

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::pc), emu->mod_manager.dylinker->entry_point);
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::sp), emu->process.kernel_args_pointer);

        uint64_t header{};
        emu->memory.read_memory(emu->process.kernel_args_pointer, &header, sizeof(header));
        EXPECT_EQ(header, emu->mod_manager.executable->image_base);
    }

    TEST(DyldHandoff, ThePrivateCachePathIsForcedIntoTheGuestEnvironment)
    {
        const sogen::test::temp_directory root{"handoff-env"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = {0xD4200000u};
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());
        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {"HOME=/Users/x"}));

        const auto& envp = emu->process.envp;
        EXPECT_NE(std::ranges::find(envp, "DYLD_SHARED_REGION=private"), envp.end());
        EXPECT_NE(std::ranges::find(envp, "DYLD_PAGEIN_LINKING=0"), envp.end());
        EXPECT_NE(std::ranges::find(envp, "HOME=/Users/x"), envp.end());

        EXPECT_NE(emu->process.apple_strings.dyld_file.object_id, 0u);
        EXPECT_NE(emu->process.apple_strings.executable_file.object_id, emu->process.apple_strings.dyld_file.object_id);
    }

    TEST(DyldHandoff, AMissingDylinkerIsReportedRatherThanThrown)
    {
        const sogen::test::temp_directory root{"handoff-missing"};

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());

        EXPECT_FALSE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::image_load_failure);
        EXPECT_FALSE(emu->last_stop_detail().empty());
    }

    // The module manager used to build its own guest_file_system from the emulation root, so the
    // passthrough prefix for the directory the sample was launched from existed on the emulator's
    // file system and not on the loader's. The two must resolve every path identically.
    TEST(DyldHandoff, TheLoaderAndTheEmulatorShareOneFileSystem)
    {
        const sogen::test::temp_directory root{"handoff-fs"};
        const sogen::test::temp_directory outside{"handoff-outside"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = {0xD4200000u};
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(outside.path() / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());
        ASSERT_TRUE(emu->load_dyld_application(outside.path() / "hello", {}, {}));

        const auto sibling = (outside.path() / "sidecar.dylib").generic_string();
        EXPECT_EQ(emu->mod_manager.resolve_guest_path(sibling), emu->file_sys.translate(sibling))
            << "the loader resolves a path beside the sample differently from the emulator";

        EXPECT_EQ(emu->mod_manager.resolve_guest_path("/usr/lib/dyld"), emu->file_sys.translate("/usr/lib/dyld"));
    }

    // The stack is laid out twice: once by setup_for_dyld before any Mach port exists, and again once
    // th_port and the two file identities are known. What the guest actually reads has to be the second
    // one, so this walks the apple[] vector out of guest memory rather than trusting process.apple.
    TEST(DyldHandoff, TheStackTheGuestSeesCarriesTheFinalAppleStrings)
    {
        const sogen::test::temp_directory root{"handoff-final"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = {0xD4200000u};
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());
        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        const auto sp = emu->emu().reg(sogen::arm64_register::sp);
        ASSERT_EQ(sp, emu->process.kernel_args_pointer) << "the guest starts on the stack the second pass built";

        const auto read_word = [&](const uint64_t address) {
            uint64_t value{};
            emu->memory.read_memory(address, &value, sizeof(value));
            return value;
        };

        const auto read_string = [&](uint64_t address) {
            std::string value{};
            for (char character{};; ++address)
            {
                emu->memory.read_memory(address, &character, sizeof(character));
                if (character == '\0')
                {
                    return value;
                }

                value.push_back(character);
            }
        };

        EXPECT_EQ(read_word(sp), emu->mod_manager.executable->image_base);

        const auto argc = read_word(sp + 8);
        const auto argv_base = sp + 16;
        const auto envp_base = argv_base + 8 * (argc + 1);

        auto apple_base = envp_base;
        while (read_word(apple_base) != 0)
        {
            apple_base += 8;
        }
        apple_base += 8;

        ASSERT_EQ(emu->process.apple.size(), 12u);
        for (size_t i = 0; i < emu->process.apple.size(); ++i)
        {
            EXPECT_EQ(read_string(read_word(apple_base + 8 * i)), emu->process.apple[i]) << "apple[" << i << "]";
        }

        EXPECT_NE(emu->process.apple_strings.th_port, 0u) << "libpthread reads the main thread's port out of apple[]";

        std::array<char, 32> port_text{};
        std::snprintf(port_text.data(), port_text.size(), "th_port=0x%x", emu->process.apple_strings.th_port);
        EXPECT_EQ(emu->process.apple[10], std::string{port_text.data()});
    }

    // The two stack passes differ in length only by the digits of th_port and the two file ids, which
    // is less than the 16-byte alignment, so for most argv they land on the same address and the final
    // sp assignment is invisible. Padding argv walks the total across the boundary.
    TEST(DyldHandoff, TheStackPointerFollowsTheSecondLayoutAtEveryAlignment)
    {
        const sogen::test::temp_directory root{"handoff-align"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = {0xD4200000u};
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        for (size_t pad = 0; pad < 24; ++pad)
        {
            const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());
            ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello", std::string(pad, 'p')}, {})) << "pad=" << pad;

            const auto sp = emu->emu().reg(sogen::arm64_register::sp);
            ASSERT_EQ(sp, emu->process.kernel_args_pointer) << "pad=" << pad;

            uint64_t argc{};
            emu->memory.read_memory(sp + 8, &argc, sizeof(argc));
            ASSERT_EQ(argc, 2u) << "pad=" << pad;

            uint64_t apple_first{};
            auto cursor = sp + 16 + 8 * (argc + 1);
            for (uint64_t word = 1; word != 0;)
            {
                emu->memory.read_memory(cursor, &word, sizeof(word));
                cursor += 8;
            }

            emu->memory.read_memory(cursor, &apple_first, sizeof(apple_first));

            std::string executable_path{};
            for (char character{};; ++apple_first)
            {
                emu->memory.read_memory(apple_first, &character, sizeof(character));
                if (character == '\0')
                {
                    break;
                }

                executable_path.push_back(character);
            }

            EXPECT_EQ(executable_path, "executable_path=/bin/hello") << "pad=" << pad;
        }
    }

    // Loads the real host dylinker the way a process does -- named by a synthetic executable's
    // LC_LOAD_DYLINKER -- rather than in the executable slot. dyld is a PIE with __TEXT at vmaddr 0 and
    // only the dylinker slot slides it; put it in the executable slot and the map fails outright.
    TEST(DyldHandoff, TaskDyldInfoPointsAtTheDylinkersAllImageInfoSection)
    {
        const std::filesystem::path host_dyld{MACOS_DYLD_HOST_PATH};
        if (!std::filesystem::is_regular_file(host_dyld))
        {
            GTEST_SKIP() << "no host /usr/lib/dyld";
        }

        const sogen::test::temp_directory scratch{"all-image-info"};

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = MACOS_DYLD_HOST_PATH;
        executable.code = {0xD4200000u};
        macos_test::write_image(scratch.path() / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{"/"});
        ASSERT_TRUE(emu->load_dyld_application(scratch.path() / "hello", {"/bin/hello"}, {})) << emu->last_stop_detail();

        ASSERT_NE(emu->mod_manager.dylinker, nullptr);

        const sogen::macos_mapped_section* section = nullptr;
        for (const auto& candidate : emu->mod_manager.dylinker->sections)
        {
            if (candidate.name == sogen::MACOS_DYLD_ALL_IMAGE_INFO_SECTION)
            {
                section = &candidate;
                break;
            }
        }

        ASSERT_NE(section, nullptr) << "the host dylinker has no __all_image_info section";
        EXPECT_EQ(emu->mach.all_image_info_address, section->start);
        EXPECT_EQ(emu->mach.all_image_info_size, section->length);
        EXPECT_NE(emu->mach.all_image_info_address, 0u);
        EXPECT_GE(emu->mach.all_image_info_address, emu->mod_manager.dylinker->image_base);

        EXPECT_LT(emu->mach.all_image_info_address + emu->mach.all_image_info_size,
                  emu->mod_manager.dylinker->image_start + emu->mod_manager.dylinker->size_of_image);

        // Located by name, never by segment: it moved from __DATA to __DATA_DIRTY on macOS 26. On this
        // host (build 25G76) it sits at image offset 0xc4000 with size 0x170, which is not asserted
        // because it is a property of the installed dylinker rather than of this code.
        EXPECT_TRUE(section->segment_name == "__DATA_DIRTY" || section->segment_name == "__DATA")
            << "unexpected segment " << section->segment_name;
        EXPECT_GE(emu->mach.all_image_info_size, sizeof(uint64_t) * 4) << "too small to be dyld_all_image_infos";
    }

    std::vector<uint32_t> kernel_args_probe()
    {
        return {
            0xF94003F3u, 0xF94007F4u, 0x910043F5u, 0x8B140EB6u, 0x910022D6u, 0xAA1603F7u, 0xF84086F8u, 0xB5FFFFF8u,
            0xD2800020u, 0xAA1303E1u, 0xD2800082u, 0xD2800090u, 0xD4001001u, 0xF94002E1u, 0xD2800202u, 0xD2800090u,
            0xD2800020u, 0xD4001001u, 0xD2800000u, 0xD2800030u, 0xD4001001u, 0xD4200000u,
        };
    }

    TEST(DyldHandoff, ASyntheticDylinkerReadsTheKernelArgsAndReachesExit)
    {
        const sogen::test::temp_directory root{"handoff-probe"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = kernel_args_probe();
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());

        std::string captured{};
        emu->callbacks.on_stdout = [&captured](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello"}, {}));

        emu->start(4096);

        ASSERT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);

        ASSERT_EQ(captured.size(), 20u);

        // The first four bytes are the executable's mach header read through [sp]; MH_MAGIC_64 is
        // little-endian 0xfeedfacf.
        EXPECT_EQ(static_cast<uint8_t>(captured[0]), 0xCFu);
        EXPECT_EQ(static_cast<uint8_t>(captured[1]), 0xFAu);
        EXPECT_EQ(static_cast<uint8_t>(captured[2]), 0xEDu);
        EXPECT_EQ(static_cast<uint8_t>(captured[3]), 0xFEu);

        EXPECT_EQ(captured.substr(4), "executable_path=");
    }

    TEST(DyldHandoff, TheSyntheticDylinkerSeesTheArgumentCountItWasGiven)
    {
        const sogen::test::temp_directory root{"handoff-argc"};

        macos_test::macho_image_spec dylinker{};
        dylinker.file_type = sogen::macho::MH_DYLINKER;
        dylinker.text_vmaddr = 0;
        dylinker.page_zero_size = 0;
        dylinker.code = kernel_args_probe();
        macos_test::write_image(root.path() / "usr" / "lib" / "dyld", macos_test::build_macho_image(dylinker));

        macos_test::macho_image_spec executable{};
        executable.dylinker_path = "/usr/lib/dyld";
        executable.code = {0xD4200000u};
        macos_test::write_image(root.path() / "bin" / "hello", macos_test::build_macho_image(executable));

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root.path());

        std::string captured{};
        emu->callbacks.on_stdout = [&captured](const std::string_view data) { captured.append(data); };

        ASSERT_TRUE(emu->load_dyld_application("/bin/hello", {"/bin/hello", "a", "b", "c"}, {"X=1", "Y=2"}));
        emu->start(4096);

        // A wrong argc makes the envp walk land inside the argv block and the apple pointer come out of
        // the string area, so this assertion is what actually pins the 8·argc arithmetic.
        ASSERT_EQ(captured.size(), 20u) << emu->last_stop_detail();
        EXPECT_EQ(captured.substr(4), "executable_path=");
    }
}
