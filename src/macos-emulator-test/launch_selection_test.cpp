#include <gtest/gtest.h>

#include "dyld_fixture.hpp"
#include "fixture_utils.hpp"
#include "macos_test_utils.hpp"

namespace
{
    bool host_dyld_available()
    {
        return std::filesystem::is_regular_file(MACOS_DYLD_HOST_PATH);
    }

    std::filesystem::path write_static_executable(const sogen::test::temp_directory& directory)
    {
        macos_test::macho_image_spec executable{};
        executable.code = {
            0xD2800000u, // mov x0, #0
            0xD2800030u, // mov x16, #1   (exit)
            0xD4001001u, // svc #0x80
        };

        const auto path = directory.path() / "bin" / "static";
        macos_test::write_image(path, macos_test::build_macho_image(executable));
        return path;
    }

    std::filesystem::path write_dynamic_root(const sogen::test::temp_directory& directory)
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

    // Which pointer-authentication mode the process runs in is decided by the main executable and
    // nothing else, before any guest code runs. Getting it from the running image rather than assuming
    // arm64e is what lets an ordinary arm64 binary use the arm64e shared cache at all.
    TEST(LaunchSelection, ThePointerAuthenticationModeComesFromTheMainExecutable)
    {
        const auto mode_for = [](const uint32_t cpu_subtype) {
            const sogen::test::temp_directory scratch{"launch-pac"};

            macos_test::macho_image_spec executable{};
            executable.cpu_subtype = cpu_subtype;
            executable.code = {
                0xD2800000u, // mov x0, #0
                0xD2800030u, // mov x16, #1   (exit)
                0xD4001001u, // svc #0x80
            };

            macos_test::write_image(scratch.path() / "bin" / "static", macos_test::build_macho_image(executable));

            const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), scratch.path());
            EXPECT_TRUE(emu->load_executable("/bin/static", {"/bin/static"}, {}));
            return emu->pointer_authentication;
        };

        EXPECT_FALSE(mode_for(sogen::macho::CPU_SUBTYPE_ARM64_ALL)) << "a plain arm64 process runs with the keys off";
        EXPECT_TRUE(mode_for(sogen::macho::CPU_SUBTYPE_ARM64E)) << "an arm64e process runs with them on";
    }

    TEST(LaunchSelection, StaticExecutableStartsAtItsOwnEntryPoint)
    {
        const sogen::test::temp_directory scratch{"launch-static"};
        const auto executable = write_static_executable(scratch);

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), scratch.path());
        ASSERT_TRUE(emu->load_executable("/bin/static", {"/bin/static"}, {}));

        EXPECT_EQ(emu->emu().read_instruction_pointer(), emu->mod_manager.executable->entry_point)
            << "a static image has no dylinker to hand control to";

        emu->start(1000);
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0);
    }

    // The distinction that matters: a dynamic image must begin inside dyld, not at its own entry, or
    // nothing is linked and the first call into libSystem faults.
    TEST(LaunchSelection, DynamicExecutableStartsInsideTheDylinker)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"launch-dynamic"};
        const auto root = write_dynamic_root(scratch);

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);
        ASSERT_TRUE(emu->load_executable("/bin/hello", {"/bin/hello"}, {}));

        ASSERT_NE(emu->mod_manager.dylinker, nullptr);
        EXPECT_EQ(emu->emu().read_instruction_pointer(), emu->mod_manager.dylinker->entry_point);
        EXPECT_NE(emu->emu().read_instruction_pointer(), emu->mod_manager.executable->entry_point);
    }

    // dyld reads the pid and, seeing 1, believes it is launchd on a booting system: it runs libignition,
    // tries to mount preboot, and when that fails reports the error to /dev/console after closing the
    // descriptors it thinks it inherited. The guest then has no stdout. Nothing about the emulator
    // requires pid 1, and a real analysis target never has it.
    TEST(LaunchSelection, DyldLaunchLeavesTheStandardDescriptorsOpen)
    {
        if (!host_dyld_available())
        {
            GTEST_SKIP() << "no host " << MACOS_DYLD_HOST_PATH;
        }

        const sogen::test::temp_directory scratch{"launch-descriptors"};
        const auto root = write_dynamic_root(scratch);

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), root);
        ASSERT_NE(emu->process.pid, 1u) << "dyld takes its launchd boot path when the guest claims pid 1";

        ASSERT_TRUE(emu->load_executable("/bin/hello", {"/bin/hello"}, {}));
        emu->start(400'000'000);

        EXPECT_NE(emu->process.fds.get(0), nullptr) << "dyld tore down stdin";
        EXPECT_NE(emu->process.fds.get(1), nullptr) << "dyld tore down stdout, so guest output goes nowhere";
        EXPECT_NE(emu->process.fds.get(2), nullptr) << "dyld tore down stderr";
    }

    TEST(LaunchSelection, MissingExecutableFails)
    {
        const sogen::test::temp_directory scratch{"launch-missing"};
        std::filesystem::create_directories(scratch.path() / "bin");

        const auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), scratch.path());
        EXPECT_FALSE(emu->load_executable("/bin/absent", {"/bin/absent"}, {}));
    }
}
