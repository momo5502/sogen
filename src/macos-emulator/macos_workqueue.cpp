#include "std_include.hpp"
#include "macos_workqueue.hpp"

#include "macos_emulator.hpp"
#include "macos_thread.hpp"
#include "host_range_reader.hpp"

#include <cinttypes>

namespace sogen
{
    uint64_t macos_workqueue::resolve_wqthread_entry(macos_emulator& emu)
    {
        if (this->wqthread_entry_ != 0)
        {
            return this->wqthread_entry_;
        }

        if (!emu.shared_cache_host_path.empty() && !this->cache_)
        {
            try
            {
                // The same reader shape macos_ui_state::bind uses, for the same reason: in the browser
                // the cache is lazily fetched and must not be opened twice.
                this->cache_.emplace(
                    dyld_shared_cache_reader::parse(emu.shared_cache_host_path, make_host_range_cache_opener(default_host_range_reader())));
                this->symbols_.emplace(*this->cache_);
            }
            catch (const std::exception& e)
            {
                emu.log.warn("worker spawn needs the shared cache and could not read %s: %s\n", emu.shared_cache_host_path.string().c_str(),
                             e.what());
            }
        }

        if (this->symbols_)
        {
            if (const auto address = this->symbols_->find_export(MACOS_PTHREAD_IMAGE_PATH, "_start_wqthread"))
            {
                this->wqthread_entry_ = *address;
                return *address;
            }
        }

        // Measured 2026-08-27 as a regular export of this build's libsystem_pthread, so this is a known
        // address on the build the guest's cache comes from -- but a different cache would make it a lie,
        // which is why it warns.
        emu.log.warn("_start_wqthread could not be resolved from the shared cache; falling back to the measured export 0x%" PRIx64 "\n",
                     MACOS_START_WQTHREAD_FALLBACK);
        this->wqthread_entry_ = MACOS_START_WQTHREAD_FALLBACK;
        return this->wqthread_entry_;
    }

    namespace
    {
        constexpr size_t MAX_EVENTS_PER_WAKEUP = MACOS_WORKQUEUE_EVENT_BUFFER_OFFSET / sizeof(macos_kevent_qos_entry);

        void write_events(macos_emulator& emu, const uint64_t buffer, const std::vector<kevent_registration>& events)
        {
            for (size_t i = 0; i < events.size(); ++i)
            {
                macos_kevent_qos_entry entry{};
                entry.ident = events[i].ident;
                entry.filter = events[i].filter;
                entry.flags = events[i].flags;
                entry.qos = events[i].qos;
                entry.udata = events[i].udata;
                entry.fflags = events[i].fflags;
                entry.xflags = events[i].xflags;
                entry.data = events[i].data;
                std::copy(std::begin(events[i].ext), std::end(events[i].ext), std::begin(entry.ext));
                emu.memory.write_memory(buffer + i * sizeof(entry), &entry, sizeof(entry));
            }
        }
    }

    bool macos_workqueue::continue_worker(macos_emulator& emu, macos_thread& worker, const bool active)
    {
        std::vector<kevent_registration> events(MAX_EVENTS_PER_WAKEUP);
        uint64_t queue_id = 0;
        bool from_workloop = false;
        const auto count = emu.process.kqueues.drain_workq_events(events.data(), events.size(), queue_id, from_workloop);
        if (count == 0)
        {
            return false;
        }

        events.resize(count);

        const auto pthread_page = worker.stack_base + worker.stack_size;
        const auto buffer = pthread_page - MACOS_WORKQUEUE_EVENT_BUFFER_OFFSET;
        // Measured 2026-08-27 on the host: the kernel hands the workloop's kq id in the word just
        // below the event list, and __pthread_wqthread's workloop path reads it from there.
        emu.memory.write_memory(buffer - 8, &queue_id, sizeof(queue_id));
        write_events(emu, buffer, events);

        this->enter_worker(emu, worker, buffer, from_workloop ? MACOS_WQTHREAD_WORKLOOP_WAKE_FLAGS : MACOS_WQTHREAD_WORKQ_WAKE_FLAGS, count,
                           active);
        return true;
    }

    void macos_workqueue::enter_worker(macos_emulator& emu, macos_thread& worker, const uint64_t buffer, const uint32_t flags,
                                       const size_t count, const bool active)
    {
        const auto pthread_page = worker.stack_base + worker.stack_size;

        macos_saved_registers state{};
        state.pc = this->resolve_wqthread_entry(emu);
        state.sp = pthread_page;
        state.tpidrro_el0 = worker.thread_self;
        state.x[0] = pthread_page;
        state.x[1] = emu.mach.thread_self_for(worker.thread_id);
        state.x[2] = worker.stack_base;
        state.x[3] = buffer;
        state.x[4] = flags;
        state.x[5] = count;

        worker.saved_regs = state;
        worker.blocked_on_port = 0;
        if (active)
        {
            worker.restore(emu.emu());
        }

        emu.log.info("workqueue: worker thread %" PRIu64 " continued at _start_wqthread with %zu event(s)\n", worker.thread_id, count);
    }

    bool macos_workqueue::request_worker(macos_emulator& emu, const uint32_t pthread_priority)
    {
        const auto flags = macos_wqthread_flags_for_priority(pthread_priority);

        for (auto& [id, thread] : emu.process.threads)
        {
            if (thread.terminated || thread.workqueue_kport == 0 || thread.blocked_on_port != thread.workqueue_kport)
            {
                continue;
            }

            // No kevent list: a thread request is the plain workqueue path, and __pthread_wqthread runs
            // libdispatch's worker function rather than draining events.
            this->enter_worker(emu, thread, 0, flags | MACOS_WQTHREAD_REUSE_FLAG, 0, false);
            return true;
        }

        return this->spawn_worker(emu, {}, false, flags);
    }

    bool macos_workqueue::wake_parked_worker(macos_emulator& emu)
    {
        if (!emu.process.kqueues.has_workq_events())
        {
            return false;
        }

        for (auto& [id, thread] : emu.process.threads)
        {
            if (thread.terminated || thread.workqueue_kport == 0 || thread.blocked_on_port != thread.workqueue_kport)
            {
                continue;
            }

            return this->continue_worker(emu, thread, false);
        }

        return false;
    }

    bool macos_workqueue::continue_worker_with_pending_events(macos_emulator& emu, macos_thread& worker)
    {
        return this->continue_worker(emu, worker, true);
    }

    bool macos_workqueue::spawn_worker(macos_emulator& emu, const kevent_registration& request, const bool workloop, const uint32_t flags)
    {
        if (this->worker_count_ >= MACOS_WORKQUEUE_MAX_WORKERS)
        {
            emu.log.warn("workqueue thread request with the worker arena full (%zu workers); the request is left unanswered\n",
                         this->worker_count_);
            return false;
        }

        const auto entry = this->resolve_wqthread_entry(emu);

        const auto slot = MACOS_WORKQUEUE_ARENA_BASE + this->worker_count_ * MACOS_WORKQUEUE_SLOT_SIZE;
        const auto pthread_page = slot + MACOS_WORKQUEUE_STACK_SIZE;

        if (!emu.memory.allocate_memory(slot, MACOS_WORKQUEUE_SLOT_SIZE, memory_permission::read_write))
        {
            emu.log.warn("workqueue worker could not be mapped at 0x%" PRIx64 "; no thread is spawned\n", slot);
            return false;
        }

        const auto kport =
            emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::workqueue, .id = this->worker_count_ + 1});

        const auto thread_id = emu.process.create_thread(slot, MACOS_WORKQUEUE_STACK_SIZE, entry);
        auto& thread = emu.process.threads.at(thread_id);

        // Measured 2026-08-27 against a live host worker (see MACOS_WQTHREAD_SPAWN_FLAGS): the kernel
        // passes no kevent list -- x3 = NULL, x5 = 0 -- and REUSE (bit 17) stays clear so libpthread
        // runs _pthread_wqthread_setup itself, writing the TSD self-pointer at self + 0xE0.
        thread.thread_self = pthread_page + MACOS_PTHREAD_STRUCT_TO_TSD_OFFSET;
        thread.saved_regs.tpidrro_el0 = thread.thread_self;
        thread.workqueue_kport = kport;
        thread.saved_regs.x[0] = pthread_page;

        // _pthread_wqthread_setup stores x1 in the TSD slot pthread_mach_thread_np reads, so this is the
        // thread's identity and not just a port it was handed: os_unfair_lock and dispatch_once take the
        // value from there and compare it against mach_thread_self(). A pool-private port makes a worker
        // hold two identities, and the first lock it takes and releases trips libplatform's
        // "Lock unexpectedly not owned by current thread".
        thread.saved_regs.x[1] = emu.mach.thread_self_for(thread_id);
        thread.saved_regs.x[2] = slot;
        thread.saved_regs.x[3] = 0;
        thread.saved_regs.x[4] = flags != 0 ? flags : MACOS_WQTHREAD_SPAWN_FLAGS;
        thread.saved_regs.x[5] = 0;

        if (workloop)
        {
            // A workloop spawn differs (same measurement, cgsdemo's workloop registrations): the flags
            // carry the workloop bit and the kernel hands over the workloop's own registration event in
            // a buffer below the pthread page -- that event is what _pthread_wqthread runs first. The
            // word below the event list carries the workloop's kq id (the registration's ident).
            auto initial = request;
            initial.flags |= MACOS_EV_CLEAR;
            const auto buffer = pthread_page - MACOS_WORKQUEUE_EVENT_BUFFER_OFFSET;
            const auto workloop_id = request.ident;
            emu.memory.write_memory(buffer - 8, &workloop_id, sizeof(workloop_id));
            write_events(emu, buffer, {initial});
            thread.saved_regs.x[3] = buffer;
            thread.saved_regs.x[4] = MACOS_WQTHREAD_WORKLOOP_SPAWN_FLAGS;
            thread.saved_regs.x[5] = 1;
        }

        ++this->worker_count_;
        emu.log.info("workqueue: worker thread %" PRIu64 " spawned at _start_wqthread 0x%" PRIx64 " (kport 0x%x)\n", thread_id, entry,
                     kport);
        emu.callbacks.on_thread_create(thread_id, entry, pthread_page);
        return true;
    }
}
