#pragma once

#include <gtest/gtest.h>
#include <windows_emulator.hpp>

namespace test
{
	inline void assert_terminated_with_status(const windows_emulator& win_emu, const NTSTATUS status)
	{
		ASSERT_TRUE(win_emu.process().exit_status.has_value());
		ASSERT_EQ(*win_emu.process().exit_status, status);
	}

	inline void assert_terminated_successfully(const windows_emulator& win_emu)
	{
		assert_terminated_with_status(win_emu, STATUS_SUCCESS);
	}
}
