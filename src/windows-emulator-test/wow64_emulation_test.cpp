#include <backend_selection.hpp>
#include <windows_emulator.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace sogen::test
{
    class Wow64EmulationTest : public testing::TestWithParam<uint32_t>
    {
    };

    TEST_P(Wow64EmulationTest, GuestMemorySseThreadsAndExceptionsComplete)
    {
        const auto* root = std::getenv("EMULATOR_ROOT");
        const auto* sample = std::getenv("EMULATOR_WOW64_SAMPLE");
        if (!root || !*root || !sample || !*sample)
        {
            GTEST_SKIP() << "Set EMULATOR_ROOT and EMULATOR_WOW64_SAMPLE to run the x86 guest sample";
        }
        ASSERT_TRUE(std::filesystem::is_regular_file(sample));
        emulator_settings settings{};
        settings.emulation_root = root;
        settings.root_mode = filesystem_mode::read_only;
        settings.mounts.push_back({
            .host_path = std::filesystem::absolute(sample),
            .guest_path = windows_path{"C:\\wow64-test-sample.exe"},
            .mode = filesystem_mode::read_only,
        });
        application_settings application{.application = "C:\\wow64-test-sample.exe"};
        application.arguments.emplace_back(u"wow64-argument");
        if (GetParam() == 37)
        {
            application.arguments.emplace_back(u"--exit-37");
        }
        std::string output;
        emulator_callbacks callbacks{};
        callbacks.on_stdout = [&](const std::string_view text) { output.append(text); };
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator emu{create_x86_64_emulator_from_environment(), std::move(application), settings, std::move(callbacks),
                             std::move(interfaces)};
        emu.log.disable_output(true);
        emu.start();

        EXPECT_TRUE(emu.process.is_wow64_process);
        ASSERT_TRUE(emu.process.exit_status.has_value()) << output;
        EXPECT_EQ(*emu.process.exit_status, GetParam()) << output;
        EXPECT_NE(output.find("WOW64_PROBE_PASS"), std::string::npos) << output;
    }

    INSTANTIATE_TEST_SUITE_P(ExitStatus, Wow64EmulationTest, testing::Values(0u, 37u));
}
