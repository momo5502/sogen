#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include <utils/host_process.hpp>
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
            // The host process of the emulator whose guest thread owns this, so a process that is killed or crashes
            // while holding a mutant does not leave every other process waiting on it forever.
            uint32_t owner_host_process;
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

            // The lock word holds the host process id of its holder rather than a flag, so a process killed in the
            // middle of an update can be recovered instead of leaving every survivor spinning. Taking it over is
            // safe because the dead holder cannot still be writing.
            const auto self = utils::current_host_process_id();
            for (uint32_t holder = 0; !lock.compare_exchange_weak(holder, self, std::memory_order_acquire);)
            {
                if (holder != 0 && !utils::host_process_alive(holder))
                {
                    if (lock.compare_exchange_strong(holder, self, std::memory_order_acquire))
                    {
                        break;
                    }
                }

                holder = 0;
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

            // An owner whose emulator is gone never releases, so its ownership is handed on as abandoned, which is
            // what Windows reports when the owning process dies.
            if (state.values.locked_count > 0 && state.values.owner_host_process != 0 &&
                !utils::host_process_alive(state.values.owner_host_process))
            {
                // Abandoned goes first: a second death in between must not leave an unowned mutant that nobody
                // reports as abandoned.
                state.values.abandoned = 1;
                state.values.owner = 0;
                state.values.owner_host_process = 0;
                state.values.locked_count = 0;
            }

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
