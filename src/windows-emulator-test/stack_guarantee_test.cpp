#include "emulation_test_utils.hpp"

#include <gtest/gtest.h>

namespace sogen::test
{
    namespace
    {
        class StackGuaranteeTest : public testing::TestWithParam<bool>
        {
        };
    }

    TEST_P(StackGuaranteeTest, NewThreadHasNoExplicitStackGuarantee)
    {
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        interfaces.audio = std::make_unique<null_audio_backend>();
        interfaces.console = std::make_unique<null_console_backend>();
        windows_emulator emu{create_x86_64_emulator_from_environment(),
                             emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root(), .load_registry = false},
                             emulator_callbacks{}, std::move(interfaces)};
        emu.process.is_wow64_process = GetParam();
        emulator_thread thread{emu.memory, emu.process, 0, 0, 0x100000, 0, 8, true};
        ASSERT_TRUE(thread.teb64.has_value());
        EXPECT_EQ(thread.teb64->read().GuaranteedStackBytes, 0u);
        if (GetParam())
        {
            ASSERT_TRUE(thread.teb32.has_value());
            EXPECT_EQ(thread.teb32->read().GuaranteedStackBytes, 0u);
        }
    }

    INSTANTIATE_TEST_SUITE_P(NativeAndWow64, StackGuaranteeTest, testing::Bool());
}
