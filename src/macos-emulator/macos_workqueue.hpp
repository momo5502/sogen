#pragma once

#include "std_include.hpp"

#include "macos_kqueue.hpp"

#include "module/macos_cache_symbols.hpp"
#include "module/dyld_shared_cache.hpp"

namespace sogen
{
    class macos_emulator;
    struct macos_thread;

 // Stage A: a workq or
    // workloop kevent registration is libdispatch asking the kernel for a worker thread, and this is the
    // piece that answers. Each request spawns one real guest thread at libpthread's _start_wqthread with
    // the register contract __pthread_wqthread reads, so the guest's own libdispatch runs its queues.
    class macos_workqueue
    {
      public:
        bool spawn_worker(macos_emulator& emu, const kevent_registration& request, bool workloop, uint32_t flags = 0);

        // WQOPS_QUEUE_REQTHREADS: a worker asked for with no kevent behind it. A pool-parked worker is
        // handed back first, because the arena is finite and a run loop asks for threads repeatedly.
        // The pthread_priority_t the request carries decides which root queue the worker will drain, so
        // it has to reach the flags word rather than being dropped for a constant.
        bool request_worker(macos_emulator& emu, uint32_t pthread_priority);

        // The wake half of the pool contract: a pool-parked worker is re-entered at _start_wqthread
        // (REUSE set) with the pending workq/workloop events written into its kevent buffer. Returns
        // false when no parked worker exists or no events are pending.
        bool wake_parked_worker(macos_emulator& emu);

        // The same continuation for the thread that is parking right now: a worker that returns to the
        // pool while events are pending never sleeps -- it is continued with them instead.
        bool continue_worker_with_pending_events(macos_emulator& emu, macos_thread& worker);

      private:
        uint64_t resolve_wqthread_entry(macos_emulator& emu);
        bool continue_worker(macos_emulator& emu, macos_thread& worker, bool active);
        void enter_worker(macos_emulator& emu, macos_thread& worker, uint64_t buffer, uint32_t flags, size_t count, bool active);

        std::optional<dyld_shared_cache_reader> cache_{};
        std::optional<macos_cache_symbols> symbols_{};
        uint64_t wqthread_entry_{};
        size_t worker_count_{};
    };
}
