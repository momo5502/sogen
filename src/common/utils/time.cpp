#include "time.hpp"
#include <cstring>

namespace utils
{
    std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(const LARGE_INTEGER delay_interval)
    {
        if (delay_interval.QuadPart <= 0)
        {
            const auto relative_time = -delay_interval.QuadPart;
            const auto relative_ticks_in_ms = relative_time / 10;
            const auto relative_fraction_ns = (relative_time % 10) * 100;
            const auto relative_duration =
                std::chrono::microseconds(relative_ticks_in_ms) + std::chrono::nanoseconds(relative_fraction_ns);

            return std::chrono::steady_clock::now() + relative_duration;
        }

        const auto delay_seconds_since_1601 = delay_interval.QuadPart / HUNDRED_NANOSECONDS_IN_ONE_SECOND;
        const auto delay_fraction_ns = (delay_interval.QuadPart % HUNDRED_NANOSECONDS_IN_ONE_SECOND) * 100;

        const auto delay_seconds_since_1970 = delay_seconds_since_1601 - EPOCH_DIFFERENCE_1601_TO_1970_SECONDS;

        const auto target_time = std::chrono::system_clock::from_time_t(delay_seconds_since_1970) +
                                 std::chrono::nanoseconds(delay_fraction_ns);

        const auto now_system = std::chrono::system_clock::now();

        const auto duration_until_target =
            std::chrono::duration_cast<std::chrono::microseconds>(target_time - now_system);

        return std::chrono::steady_clock::now() + duration_until_target;
    }

    KSYSTEM_TIME convert_to_ksystem_time(const std::chrono::system_clock::time_point& tp)
    {
        const auto duration = tp.time_since_epoch();
        const auto ns_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

        const auto total_ticks = ns_duration.count() / 100 + WINDOWS_EPOCH_DIFFERENCE;

        KSYSTEM_TIME time{};
        time.LowPart = static_cast<uint32_t>(total_ticks);
        time.High1Time = static_cast<int32_t>(total_ticks >> 32);
        time.High2Time = time.High1Time;

        return time;
    }

    void convert_to_ksystem_time(volatile KSYSTEM_TIME* dest, const std::chrono::system_clock::time_point& tp)
    {
        const auto time = convert_to_ksystem_time(tp);
        memcpy(const_cast<KSYSTEM_TIME*>(dest), &time, sizeof(*dest));
    }

    std::chrono::system_clock::time_point convert_from_ksystem_time(const KSYSTEM_TIME& time)
    {
        auto totalTicks = (static_cast<int64_t>(time.High1Time) << 32) | time.LowPart;
        totalTicks -= WINDOWS_EPOCH_DIFFERENCE;

        const auto duration = std::chrono::system_clock::duration(totalTicks * 100);
        return std::chrono::system_clock::time_point(duration);
    }

    std::chrono::system_clock::time_point convert_from_ksystem_time(const volatile KSYSTEM_TIME& time)
    {
        return convert_from_ksystem_time(*const_cast<const KSYSTEM_TIME*>(&time));
    }

#ifndef OS_WINDOWS
    using __time64_t = int64_t;
#endif

    LARGE_INTEGER convert_unix_to_windows_time(const __time64_t unix_time)
    {
        LARGE_INTEGER windows_time{};
        windows_time.QuadPart = (unix_time + EPOCH_DIFFERENCE_1601_TO_1970_SECONDS) * HUNDRED_NANOSECONDS_IN_ONE_SECOND;
        return windows_time;
    }
}
