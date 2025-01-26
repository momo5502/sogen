#pragma once

#include <cstdlib>
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
    inline bool enable_verbose_logging()
    {
        const auto* env = getenv("EMULATOR_VERBOSE");
        return env && (env == "1"sv || env == "true"sv);
    }

    inline std::filesystem::path get_emulator_root()
    {
        const auto* env = getenv("EMULATOR_ROOT");
        if (!env)
        {
            throw std::runtime_error("No EMULATOR_ROOT set!");
        }

        return env;
    }

    inline windows_emulator create_sample_emulator(emulator_settings settings, emulator_callbacks callbacks = {})
    {
        const auto is_verbose = enable_verbose_logging();

        if (is_verbose)
        {
            settings.disable_logging = false;
            settings.verbose_calls = true;
        }

        settings.application = "c:/test-sample.exe";
        settings.emulation_root = get_emulator_root();
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

    inline void bisect_emulation(windows_emulator& emu)
    {
        utils::buffer_serializer start_state{};
        emu.serialize(start_state);

        emu.start();
        const auto limit = emu.process().executed_instructions;

        const auto reset_emulator = [&] {
            utils::buffer_deserializer deserializer{start_state.get_buffer()};
            emu.deserialize(deserializer);
        };

        const auto get_state_for_count = [&](const size_t count) {
            reset_emulator();
            emu.start({}, count);

            utils::buffer_serializer state{};
            emu.serialize(state);
            return state;
        };

        const auto has_diff_after_count = [&](const size_t count) {
            const auto s1 = get_state_for_count(count);
            const auto s2 = get_state_for_count(count);

            return s1.get_diff(s2).has_value();
        };

        if (!has_diff_after_count(limit))
        {
            puts("Emulation has no diff");
        }

        auto lower_bound = 0ULL;
        auto upper_bound = limit;

        printf("Bounds: %" PRIx64 " - %" PRIx64 "\n", lower_bound, upper_bound);

        while (lower_bound + 1 < upper_bound)
        {
            const auto diff = (upper_bound - lower_bound);
            const auto pivot = lower_bound + (diff / 2);

            const auto has_diff = has_diff_after_count(pivot);

            auto* bound = has_diff ? &upper_bound : &lower_bound;
            *bound = pivot;

            printf("Bounds: %" PRIx64 " - %" PRIx64 "\n", lower_bound, upper_bound);
        }

        (void)get_state_for_count(lower_bound);

        const auto rip = emu.emu().read_instruction_pointer();

        printf("Diff detected after 0x%" PRIx64 " instructions at 0x%" PRIx64 " (%s)\n", lower_bound, rip,
               emu.process().mod_manager.find_name(rip));
    }
}
