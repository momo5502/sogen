#include "emulation_test_utils.hpp"

namespace sogen::test
{
    TEST(EmulationTest, BasicEmulationWorks)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }

    TEST(EmulationTest, CountedEmulationWorks)
    {
        constexpr auto count = 200000;

        auto emu = create_sample_emulator();
        emu.start(count);

        ASSERT_EQ(emu.get_executed_instructions(), count);
    }

    TEST(EmulationTest, CountedEmulationIsAccurate)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto executedInstructions = emu.get_executed_instructions();

        auto new_emu = create_sample_emulator();

        constexpr auto offset = 1;
        const auto instructionsToExecute = executedInstructions - offset;

        new_emu.start(static_cast<size_t>(instructionsToExecute));

        ASSERT_EQ(new_emu.get_executed_instructions(), instructionsToExecute);
        ASSERT_NOT_TERMINATED(new_emu);

        new_emu.start(offset);

        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);
        ASSERT_EQ(new_emu.get_executed_instructions(), executedInstructions);
    }

    TEST(EmulationTest, TopLevelWindowSampleRunsToCompletion)
    {
        auto emu = create_application_emulator(make_application_settings(u"C:\\hello-window-sample.exe"));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_EQ(emu.process.windows.size(), 1u);

        const auto it = emu.process.classes.find(u"HelloWindowSampleClass");
        ASSERT_NE(it, emu.process.classes.end());
    }

    TEST(EmulationTest, PaintWindowSampleRunsToCompletion)
    {
        auto emu = create_application_emulator(make_application_settings(u"C:\\paint-window-sample.exe"));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_EQ(emu.process.windows.size(), 1u);

        const auto it = emu.process.classes.find(u"PaintWindowSampleClass");
        ASSERT_NE(it, emu.process.classes.end());
    }

    TEST(EmulationTest, MoveWindowSampleRunsToCompletion)
    {
        auto emu = create_application_emulator(make_application_settings(u"C:\\move-window-sample.exe"));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_EQ(emu.process.windows.size(), 1u);

        const auto it = emu.process.classes.find(u"MoveWindowSampleClass");
        ASSERT_NE(it, emu.process.classes.end());
    }

    TEST(EmulationTest, RectWindowSampleRunsToCompletion)
    {
        auto emu = create_application_emulator(make_application_settings(u"C:\\rect-window-sample.exe"));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_EQ(emu.process.windows.size(), 1u);

        const auto it = emu.process.classes.find(u"RectWindowSampleClass");
        ASSERT_NE(it, emu.process.classes.end());
    }

    TEST(EmulationTest, MessageBoxSampleRunsToCompletion)
    {
        auto emu = create_application_emulator(make_application_settings(u"C:\\messagebox-sample.exe"));
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }
} // namespace sogen::test
