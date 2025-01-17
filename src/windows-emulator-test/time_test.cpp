#include "emulation_test_utils.hpp"

namespace test
{
    TEST(TimeTest, SystemTimeIsAccurate)
    {
        std::string output_buffer{};

        emulator_callbacks callbacks{
            .stdout_callback = [&output_buffer](const std::string_view data) { output_buffer.append(data); },
        };

        const emulator_settings settings{
            .arguments = {u"-time"},
            .disable_logging = true,
            .use_relative_time = false,
        };

        auto emu = create_sample_emulator(settings, callbacks);
        emu.start();

        constexpr auto prefix = "Time: "sv;

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
        ASSERT_TRUE(output_buffer.starts_with(prefix));

        output_buffer = output_buffer.substr(prefix.size());
        while (!output_buffer.empty() && (output_buffer.back() == '\n' || output_buffer.back() == '\r'))
        {
            output_buffer.pop_back();
        }

        const auto time = strtoll(output_buffer.c_str(), nullptr, 10);

        using time_point = std::chrono::system_clock::time_point;

        const time_point::duration time_duration(time);
        const time_point tp(time_duration);

        const auto now = std::chrono::system_clock::now();
        const auto diff = now - tp;

        ASSERT_LE(diff, std::chrono::hours(1));
    }
}
