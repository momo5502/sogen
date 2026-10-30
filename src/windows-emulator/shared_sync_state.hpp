#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include <utils/shared_host_buffer.hpp>

namespace sogen
{
    // The state of an event or mutant that several emulated processes hold handles to. It lives in host shared memory,
    // so each process sees the others' changes; a spinlock in the same memory makes every update atomic.
    class shared_sync_state
    {
      public:
        struct fields
        {
            uint32_t signaled;
            uint32_t locked_count;
            uint64_t owner;
            uint32_t abandoned;
        };

        static shared_sync_state create()
        {
            return shared_sync_state{utils::shared_host_buffer::create_named(sizeof(layout))};
        }

        static std::optional<shared_sync_state> open(const std::string& name)
        {
            auto buffer = utils::shared_host_buffer::open_named(name, sizeof(layout));
            if (!buffer)
            {
                return std::nullopt;
            }

            return shared_sync_state{std::move(*buffer)};
        }

        const std::string& name() const
        {
            return this->buffer_.name();
        }

        template <typename Function>
        auto access(Function&& function) const
        {
            auto& state = *reinterpret_cast<layout*>(this->buffer_.data());
            std::atomic_ref lock{state.lock};
            while (lock.exchange(1, std::memory_order_acquire) != 0)
            {
                std::this_thread::yield();
            }

            struct unlock_on_exit
            {
                std::atomic_ref<uint32_t>& lock;

                ~unlock_on_exit()
                {
                    this->lock.store(0, std::memory_order_release);
                }
            } const unlock{lock};

            return function(state.values);
        }

      private:
        struct layout
        {
            alignas(8) uint32_t lock;
            fields values;
        };

        utils::shared_host_buffer buffer_{};

        explicit shared_sync_state(utils::shared_host_buffer buffer)
            : buffer_(std::move(buffer))
        {
        }
    };
}
