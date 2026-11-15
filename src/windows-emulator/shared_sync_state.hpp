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
    namespace shared_sync_detail
    {
        // Some C++20 standard libraries, the Android NDK's among them, do not ship std::atomic_ref yet. Where it is
        // missing the compiler builtins give the same guarantees on the word in shared memory.
        inline bool take(uint32_t& word, uint32_t& expected, const uint32_t desired)
        {
#ifdef __cpp_lib_atomic_ref
            return std::atomic_ref<uint32_t>{word}.compare_exchange_strong(expected, desired, std::memory_order_acquire);
#else
            return __atomic_compare_exchange_n(&word, &expected, desired, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE);
#endif
        }

        inline void release(uint32_t& word)
        {
#ifdef __cpp_lib_atomic_ref
            std::atomic_ref<uint32_t>{word}.store(0, std::memory_order_release);
#else
            __atomic_store_n(&word, 0u, __ATOMIC_RELEASE);
#endif
        }
    }

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
            // while holding a mutant does not leave every other process waiting on it forever, and a counter that
            // every acquisition raises, because a host process id can be reused by a later owner.
            uint32_t owner_host_process;
            uint32_t owner_generation;
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

            // The lock word holds the host process id of its holder rather than a flag, so a process killed in the
            // middle of an update can be recovered instead of leaving every survivor spinning. Taking it over is
            // safe because the dead holder cannot still be writing.
            const auto self = utils::current_host_process_id();
            for (uint32_t holder = 0; !shared_sync_detail::take(state.lock, holder, self);)
            {
                if (holder != 0 && !utils::host_process_alive(holder) && shared_sync_detail::take(state.lock, holder, self))
                {
                    break;
                }

                holder = 0;
                std::this_thread::yield();
            }

            struct unlock_on_exit
            {
                uint32_t& lock;

                ~unlock_on_exit()
                {
                    shared_sync_detail::release(this->lock);
                }
            } const unlock{state.lock};

            // An owner whose emulator is gone never releases, so its ownership is handed on as abandoned, which is
            // what Windows reports when the owning process dies. Only another process's ownership is checked, and
            // the watch is opened once per owner, so the check this is reached by on every readiness pass costs a
            // single call rather than opening and closing a process handle each time.
            if (state.values.locked_count > 0 && state.values.owner_host_process != 0 && state.values.owner_host_process != self &&
                !this->owner_is_alive(state.values.owner_host_process, state.values.owner_generation))
            {
                // The order matters because any of these stores can be the last one a dying process makes: abandoned
                // first, so a free mutant is never left unreported, then the count, so a held count always still has
                // a host owner recovery can check.
                state.values.abandoned = 1;
                state.values.locked_count = 0;
                state.values.owner = 0;
                state.values.owner_host_process = 0;
            }

            return function(state.values);
        }

      private:
        struct layout
        {
            alignas(8) uint32_t lock;
            fields values;
        };

        // The watch is replaced whenever ownership changes, not only when the process id differs: a released and
        // exited owner whose id a later owner is given would otherwise be answered from the dead process's handle.
        bool owner_is_alive(const uint32_t process_id, const uint32_t generation) const
        {
            if (this->owner_watch_.process_id() != process_id || this->watched_generation_ != generation)
            {
                this->owner_watch_ = utils::host_process_watch{process_id};
                this->watched_generation_ = generation;
            }

            return this->owner_watch_.alive();
        }

        mutable utils::host_process_watch owner_watch_{};
        mutable uint32_t watched_generation_{};

        utils::shared_host_buffer buffer_{};

        explicit shared_sync_state(utils::shared_host_buffer buffer)
            : buffer_(std::move(buffer))
        {
        }
    };
}
