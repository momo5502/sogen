#pragma once

#include <memory>
#include <optional>
#include <utility>

#include <utils/time.hpp>

namespace sogen
{
    class scheduler_clock : public utils::clock
    {
        struct snapshot
        {
            const scheduler_clock* owner{};
            std::optional<steady_time_point> now{};
        };

        static snapshot& current()
        {
            static thread_local snapshot value{};
            return value;
        }

      public:
        class scope
        {
          public:
            explicit scope(scheduler_clock& owner)
                : previous_(current())
            {
                current() = {.owner = &owner, .now = {}};
            }

            ~scope()
            {
                current() = this->previous_;
            }

            scope(const scope&) = delete;
            scope& operator=(const scope&) = delete;
            scope(scope&&) = delete;
            scope& operator=(scope&&) = delete;

          private:
            snapshot previous_;
        };

        explicit scheduler_clock(std::unique_ptr<utils::clock> inner)
            : inner_(std::move(inner))
        {
        }

        system_time_point system_now() override
        {
            return this->inner_->system_now();
        }

        steady_time_point steady_now() override
        {
            auto& snapshot = current();
            if (snapshot.owner != this)
            {
                return this->inner_->steady_now() + this->steady_offset_;
            }

            if (!snapshot.now)
            {
                snapshot.now = this->inner_->steady_now() + this->steady_offset_;
            }

            return *snapshot.now;
        }

        uint64_t timestamp_counter() override
        {
            return this->inner_->timestamp_counter() + this->counter_offset_;
        }

        // Steady time and the timestamp counter go on from saved values: a loaded snapshot continues the time of its save,
        // as a Hyper-V partition's time stands still while it is in a saved state and a restored partition's TSC offset
        // continues its TSC. A guest that reads the TSC itself (no RDTSC interception) still reads the host's.
        void continue_time_at(const steady_time_point steady, const uint64_t counter)
        {
            this->steady_offset_ = steady - this->inner_->steady_now();
            this->counter_offset_ = counter - this->inner_->timestamp_counter();
        }

      private:
        std::unique_ptr<utils::clock> inner_{};
        steady_duration steady_offset_{};
        uint64_t counter_offset_{};
    };
}
