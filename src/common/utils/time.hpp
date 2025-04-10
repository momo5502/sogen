#pragma once

#include <chrono>

#include "../platform/platform.hpp"
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif

constexpr auto HUNDRED_NANOSECONDS_IN_ONE_SECOND = 10000000LL;
constexpr auto EPOCH_DIFFERENCE_1601_TO_1970_SECONDS = 11644473600LL;
constexpr auto WINDOWS_EPOCH_DIFFERENCE = EPOCH_DIFFERENCE_1601_TO_1970_SECONDS * HUNDRED_NANOSECONDS_IN_ONE_SECOND;

namespace utils
{
    struct clock
    {
        using system_time_point = std::chrono::system_clock::time_point;
        using steady_time_point = std::chrono::steady_clock::time_point;

        using system_duration = system_time_point::duration;
        using steady_duration = steady_time_point::duration;

        virtual ~clock() = default;

        virtual system_time_point system_now()
        {
            return std::chrono::system_clock::now();
        }

        virtual steady_time_point steady_now()
        {
            return std::chrono::steady_clock::now();
        }

        // Returnds the current timestamp counter value. RDTSC on x86/x64, or just time since epoch for ARM
        /// TODO: find better solution for ARM and Figure out better CPU base frequency heuristics
        virtual uint64_t timestamp_counter()
        {
#if defined(_MSC_VER)
#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_IX86)
            return __rdtsc(); // 64-bit with MSVC intrinsic
#endif

#elif defined(__x86_64__) || defined(__i386__) || defined(__amd64__) // If we are using clang or gcc
            unsigned int lo, hi;
            __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
            return ((uint64_t)hi << 32) | lo;
#endif
            return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) *
                   3.8; // should be base cpu frequency here;
        }
    };

    class tick_clock : public clock
    {
      public:
        tick_clock(const uint64_t frequency = 1, const system_time_point system_start = {},
                   const steady_time_point steady_start = {})
            : frequency_(frequency),
              system_start_(system_start),
              steady_start_(steady_start)
        {
            if (this->frequency_ == 0)
            {
                throw std::invalid_argument("Frequency can not be 0");
            }
        }

        system_time_point system_now() override
        {
            return this->now(this->system_start_);
        }

        steady_time_point steady_now() override
        {
            return this->now(this->steady_start_);
        }

        virtual uint64_t ticks() = 0;

        uint64_t get_frequency() const
        {
            return this->frequency_;
        }

      private:
        uint64_t frequency_{1};
        system_time_point system_start_{};
        steady_time_point steady_start_{};

        template <typename TimePoint>
        TimePoint now(const TimePoint start)
        {
            using duration = typename TimePoint::duration;

            const auto passed_ticks = this->ticks();
            const auto passed_time = duration(passed_ticks * duration::period::den / this->frequency_);

            return start + passed_time;
        }
    };

    std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(clock& c, LARGE_INTEGER delay_interval);

    KSYSTEM_TIME convert_to_ksystem_time(const std::chrono::system_clock::time_point& tp);
    void convert_to_ksystem_time(volatile KSYSTEM_TIME* dest, const std::chrono::system_clock::time_point& tp);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const KSYSTEM_TIME& time);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const volatile KSYSTEM_TIME& time);
#ifndef OS_WINDOWS
    using __time64_t = int64_t;
#endif
    LARGE_INTEGER convert_unix_to_windows_time(__time64_t unix_time);
}
