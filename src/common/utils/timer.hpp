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
            return this->elapsed() > duration;
        }

        typename Clock::duration elapsed() const
        {
            const auto now = Clock::now();
            return now - this->point_;
        }

      private:
        typename Clock::time_point point_{Clock::now()};
    };
}
