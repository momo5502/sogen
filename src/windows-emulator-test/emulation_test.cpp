#include <gtest/gtest.h>
#include <windows_emulator.hpp>


namespace test
{
	TEST(EmulationTest, BasicEmulationWorks)
	{
		windows_emulator emu{"./test-sample.exe"};
		emu.logger.disable_output(true);
		emu.start();

		ASSERT_TRUE(emu.process().exit_status.has_value());
		ASSERT_EQ(*emu.process().exit_status, 0);
	}

	TEST(EmulationTest, CountedEmulationWorks)
	{
		constexpr auto count = 123;

		windows_emulator emu{ "./test-sample.exe" };
		emu.logger.disable_output(true);
		emu.start({}, count);

		ASSERT_EQ(emu.process().executed_instructions, count);
	}
}
