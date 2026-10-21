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
                return this->inner_->steady_now();
            }

            if (!snapshot.now)
            {
                snapshot.now = this->inner_->steady_now();
            }

            return *snapshot.now;
        }

        uint64_t timestamp_counter() override
        {
            return this->inner_->timestamp_counter();
        }

      private:
        std::unique_ptr<utils::clock> inner_{};
    };
}
