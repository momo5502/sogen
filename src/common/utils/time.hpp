#pragma once

#include <chrono>

#include "../platform/platform.hpp"

constexpr auto HUNDRED_NANOSECONDS_IN_ONE_SECOND = 10000000LL;
constexpr auto EPOCH_DIFFERENCE_1601_TO_1970_SECONDS = 11644473600LL;
constexpr auto WINDOWS_EPOCH_DIFFERENCE = EPOCH_DIFFERENCE_1601_TO_1970_SECONDS * HUNDRED_NANOSECONDS_IN_ONE_SECOND;

namespace utils
{
    std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(const LARGE_INTEGER delay_interval);
    KSYSTEM_TIME convert_to_ksystem_time(const std::chrono::system_clock::time_point& tp);
    void convert_to_ksystem_time(volatile KSYSTEM_TIME* dest, const std::chrono::system_clock::time_point& tp);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const KSYSTEM_TIME& time);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const volatile KSYSTEM_TIME& time);
#ifndef OS_WINDOWS
    using __time64_t = int64_t;
#endif
    LARGE_INTEGER convert_unix_to_windows_time(const __time64_t unix_time);
}
