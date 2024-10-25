#pragma once

#include <gtest/gtest.h>
#include <windows_emulator.hpp>

#define ASSER_TERMINATED_WITH_STATUS(win_emu, status)                   \
			do {                                                        \
				ASSERT_TRUE(win_emu.process().exit_status.has_value()); \
				ASSERT_EQ(*win_emu.process().exit_status, status);      \
			} while(false) 

#define ASSER_TERMINATED_SUCCESSFULLY(win_emu) \
			ASSER_TERMINATED_WITH_STATUS(win_emu, STATUS_SUCCESS)
