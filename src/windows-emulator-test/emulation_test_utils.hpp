#pragma once

#include <gtest/gtest.h>
#include <windows_emulator.hpp>

#define ASSERT_NOT_TERMINATED(win_emu)                             \
    do                                                             \
    {                                                              \
        ASSERT_FALSE((win_emu).process().exit_status.has_value()); \
    } while (false)

#define ASSERT_TERMINATED_WITH_STATUS(win_emu, status)            \
    do                                                            \
    {                                                             \
        ASSERT_TRUE((win_emu).process().exit_status.has_value()); \
        ASSERT_EQ(*(win_emu).process().exit_status, status);      \
    } while (false)

#define ASSERT_TERMINATED_SUCCESSFULLY(win_emu) ASSERT_TERMINATED_WITH_STATUS(win_emu, STATUS_SUCCESS)

namespace test
{
    inline windows_emulator create_sample_emulator(emulator_settings settings, emulator_callbacks callbacks = {})
    {
        settings.application = "./test-sample.exe";
        return windows_emulator{std::move(settings), std::move(callbacks)};
    }

    inline windows_emulator create_sample_emulator()
    {
        emulator_settings settings{
            .disable_logging = true,
            .use_relative_time = true,
        };

        return create_sample_emulator(std::move(settings));
    }
}
