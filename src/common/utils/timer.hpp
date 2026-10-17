#pragma once

#include <chrono>

namespace sogen
{

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

            bool has_elapsed(Clock::duration duration) const
            {
                return this->elapsed() > duration;
            }

            Clock::duration elapsed() const
            {
                const auto now = Clock::now();
                return now - this->point_;
            }

          private:
            Clock::time_point point_{Clock::now()};
        };
    }
} // namespace sogen
