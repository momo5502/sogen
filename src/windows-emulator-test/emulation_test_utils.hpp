#pragma once

#include <gtest/gtest.h>
#include <windows_emulator.hpp>

#define ASSERT_NOT_TERMINATED(win_emu)                                   \
			do {                                                         \
				ASSERT_FALSE(win_emu.process().exit_status.has_value()); \
			} while(false)


#define ASSERT_TERMINATED_WITH_STATUS(win_emu, status)                  \
			do {                                                        \
				ASSERT_TRUE(win_emu.process().exit_status.has_value()); \
				ASSERT_EQ(*win_emu.process().exit_status, status);      \
			} while(false)

#define ASSERT_TERMINATED_SUCCESSFULLY(win_emu) \
			ASSERT_TERMINATED_WITH_STATUS(win_emu, STATUS_SUCCESS)

namespace test
{
	inline windows_emulator create_sample_emulator()
	{
		const emulator_settings settings
		{
			.application = "./test-sample.exe",
			.disable_logging = true,
		};

		return windows_emulator{settings};
	}
}
