#pragma once

#include <chrono>

namespace utils
{
    template <typename Clock = std::chrono::high_resolution_clock>
    class timer
    {
      public:
        void update()
        {
            this->point_ = Clock::now();
        }

        bool has_elapsed(typename Clock::duration duration) const
        {
            const auto now = Clock::now();
            const auto diff = now - this->point_;
            return diff > duration;
        }

      private:
        typename Clock::time_point point_{Clock::now()};
    };
}
