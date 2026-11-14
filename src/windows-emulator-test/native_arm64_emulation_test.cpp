#include "emulation_test_utils.hpp"

#include <cstdlib>

#ifdef SOGEN_NATIVE_ARM64_SAMPLE
namespace sogen::test
{
    TEST(NativeArm64EmulationTest, NtWriteFileUsesSharedWindowsProcessAndThreadSetup)
    {
        const auto* root = std::getenv("EMULATOR_ARM64_ROOT");
        if (!root || !*root)
        {
            GTEST_SKIP() << "Set EMULATOR_ARM64_ROOT to an ARM64 emulation root";
        }

        emulator_settings settings{};
        settings.emulation_root = root;
        settings.intercept_rdtsc = false;
        settings.intercept_kusd = false;
        settings.use_instruction_precision = false;
        settings.mounts = {{.host_path = SOGEN_NATIVE_ARM64_SAMPLE,
                            .guest_path = windows_path{"C:\\native-arm64-hello.exe"},
                            .mode = filesystem_mode::read_only}};
        std::string output{};
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view data) { output.append(data); };
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();

        windows_emulator emu{create_arm64_emulator(backend_type::native_arm64),
                             application_settings{.application = "C:\\native-arm64-hello.exe"}, settings, std::move(callbacks),
                             std::move(interfaces)};
        emu.log.disable_output(!enable_verbose_logging());
        emu.start();

        EXPECT_EQ(emu.process.exit_status, STATUS_SUCCESS);
        EXPECT_EQ(output, "Hello World\n");
    }
}
#endif
