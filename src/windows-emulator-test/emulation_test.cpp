#include "emulation_test_utils.hpp"

namespace test
{
	TEST(EmulationTest, BasicEmulationWorks)
	{
		windows_emulator emu{"./test-sample.exe"};
		emu.logger.disable_output(true);
		emu.start();

		assert_terminated_successfully(emu);
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
