#pragma once

#include <chrono>

#include "../platform/platform.hpp"

constexpr auto HUNDRED_NANOSECONDS_IN_ONE_SECOND = 10000000LL;
constexpr auto EPOCH_DIFFERENCE_1601_TO_1970_SECONDS = 11644473600LL;
constexpr auto WINDOWS_EPOCH_DIFFERENCE = EPOCH_DIFFERENCE_1601_TO_1970_SECONDS * HUNDRED_NANOSECONDS_IN_ONE_SECOND;

namespace utils
{
    template <typename Clock>
    struct clock
    {
        using base_clock = Clock;
        using time_point = typename base_clock::time_point;
        using duration = typename base_clock::duration;

        virtual ~clock() = default;
        virtual time_point now()
        {
            return base_clock::now();
        }
    };

    template <typename Clock>
    class tick_clock : public clock<Clock>
    {
      public:
        tick_clock(const typename tick_clock::time_point start, const uint64_t frequency)
            : frequency_(frequency),
              start_(start)
        {
            if (this->frequency_ == 0)
            {
                throw std::invalid_argument("Frequency can not be 0");
            }
        }

        typename tick_clock::time_point now() override
        {
            const auto passed_ticks = this->ticks();
            const auto passed_time =
                tick_clock::duration(passed_ticks * tick_clock::duration::period::den / this->frequency_);

            return this->start_ + passed_time;
        }

        virtual uint64_t ticks() = 0;

        uint64_t get_frequency() const
        {
            return this->frequency_;
        }

      private:
        uint64_t frequency_{1};
        typename tick_clock::time_point start_{};
    };

    using system_clock = clock<std::chrono::system_clock>;
    using steady_clock = clock<std::chrono::steady_clock>;

    std::chrono::steady_clock::time_point convert_delay_interval_to_time_point(steady_clock& steady_time,
                                                                               system_clock& system_time,
                                                                               LARGE_INTEGER delay_interval);

    KSYSTEM_TIME convert_to_ksystem_time(const std::chrono::system_clock::time_point& tp);
    void convert_to_ksystem_time(volatile KSYSTEM_TIME* dest, const std::chrono::system_clock::time_point& tp);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const KSYSTEM_TIME& time);
    std::chrono::system_clock::time_point convert_from_ksystem_time(const volatile KSYSTEM_TIME& time);
#ifndef OS_WINDOWS
    using __time64_t = int64_t;
#endif
    LARGE_INTEGER convert_unix_to_windows_time(__time64_t unix_time);
}
