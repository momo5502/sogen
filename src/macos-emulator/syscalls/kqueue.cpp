#include "../std_include.hpp"
#include "../macos_emulator.hpp"
#include "../macos_kqueue.hpp"
#include "../macos_syscall_utils.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <set>

// NOLINTBEGIN(google-build-using-namespace)
namespace sogen
{

    using namespace macos_errno;

    // NOLINTEND(google-build-using-namespace)

    namespace
    {
        std::string_view describe_kevent_filter(const int16_t filter)
        {
            switch (filter)
            {
            case -1:
                return "EVFILT_READ";
            case -2:
                return "EVFILT_WRITE";
            case -3:
                return "EVFILT_AIO";
            case -4:
                return "EVFILT_VNODE";
            case -5:
                return "EVFILT_PROC";
            case -6:
                return "EVFILT_SIGNAL";
            case -7:
                return "EVFILT_TIMER";
            case -8:
                return "EVFILT_MACHPORT";
            case -9:
                return "EVFILT_FS";
            case -10:
                return "EVFILT_USER";
            case -12:
                return "EVFILT_VM";
            case -14:
                return "EVFILT_MEMORYSTATUS";
            case -15:
                return "EVFILT_EXCEPT";
            case MACOS_EVFILT_WORKLOOP:
                return "EVFILT_WORKLOOP";
            default:
                return {};
            }
        }

        // Reported once per filter, the same pattern as unimplemented MIG routine naming: accepted
        // silently would read as delivered, and a guest parked on a knote that never fires gives no
        // clue which filter it was. EVFILT_MACHPORT and EVFILT_WORKLOOP are modeled (task 6).
        void report_unmodelled_filter_once(const macos_syscall_context& c, const int16_t filter)
        {
            if (filter == MACOS_EVFILT_MACHPORT || filter == MACOS_EVFILT_WORKLOOP || filter == MACOS_EVFILT_USER ||
                filter == MACOS_EVFILT_TIMER)
            {
                return;
            }

            static std::set<int16_t> reported{};
            if (!reported.insert(filter).second)
            {
                return;
            }

            const auto name = describe_kevent_filter(filter);
            if (!name.empty())
            {
                c.emu_ref.log.warn("kevent filter %.*s (%d) is registered, but sogen has no event source for it\n",
                                   static_cast<int>(name.size()), name.data(), filter);
            }
            else
            {
                c.emu_ref.log.warn("kevent filter %d is registered, but sogen has no event source for it\n", filter);
            }
        }

        // The same two sources the rest of the emulator reads time from: CNTVCT_EL0 for the tick clock
        // every timed park is expressed in, and the host wall clock gettimeofday answers from.
        kevent_timer_clock sample_timer_clock(const macos_syscall_context& c)
        {
            const auto epoch = std::chrono::system_clock::now().time_since_epoch();
            const auto calendar_ns = std::max<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count(), 0);

            return kevent_timer_clock{
                .now = c.emu.read_system_register(3, 3, 14, 0, 2),
                .timebase_numer = c.emu_ref.mach.timebase_numer,
                .timebase_denom = c.emu_ref.mach.timebase_denom,
                .calendar_ns = static_cast<uint64_t>(calendar_ns),
            };
        }

        // filt_timervalidate (xnu bsd/kern/kern_event.c) refuses a registration naming two units at
        // once. xnu reports that per entry through EV_ERROR, which sogen does not model, so the whole
        // call is refused before anything is applied rather than silently arming a timer on a unit the
        // guest did not ask for.
        bool changelist_names_a_unit_twice(const std::vector<kevent_registration>& changes)
        {
            return std::ranges::any_of(changes, [](const kevent_registration& change) {
                return change.filter == MACOS_EVFILT_TIMER && (change.flags & MACOS_EV_DELETE) == 0 &&
                       !kevent_timer_unit_nanoseconds(change.fflags).has_value();
            });
        }

        // A deadline the guest names in the past is due the moment it is registered, so the changelist
        // itself can produce a timer event -- measured 2026-08-28: a NOTE_ABSOLUTE knote whose deadline
        // had already passed was delivered by the very next kevent with no wait at all.
        void fire_due_timers(const macos_syscall_context& c, const kevent_timer_clock& clock)
        {
            if (c.proc.kqueues.fire_due_timers(clock.now) != 0)
            {
                c.emu_ref.workqueue.wake_parked_worker(c.emu_ref);
            }
        }

        // A machport knote is level-triggered: a port with messages already queued fires at
        // registration time, not only at the next arrival.
        void fire_machport_knotes(const macos_syscall_context& c, const std::vector<kevent_registration>& changes)
        {
            size_t produced = 0;
            for (const auto& change : changes)
            {
                if (change.filter != MACOS_EVFILT_MACHPORT || (change.flags & MACOS_EV_DELETE) != 0)
                {
                    continue;
                }

                const auto* port = c.emu_ref.mach.ports.destination_of(static_cast<mach::port_name_t>(change.ident));
                if (port != nullptr && !port->queue.empty())
                {
                    produced += c.proc.kqueues.note_port_message(static_cast<uint32_t>(change.ident));
                }
            }

            if (produced != 0)
            {
                c.emu_ref.workqueue.wake_parked_worker(c.emu_ref);
            }
        }

        // Every filter xnu defines is in [-17, -1]. A value outside that is not a filter sogen has yet to
        // model -- it is a changelist sogen is reading at the wrong offset or with the wrong stride, and
        // the two need opposite fixes, so the raw entry is dumped rather than named as a filter.
        void report_undecodable_entry(const macos_syscall_context& c, const macos_kevent_qos_entry& entry, const int32_t index,
                                      const uint64_t changelist, const int32_t nchanges)
        {
            if (entry.filter <= -1 && entry.filter >= -17)
            {
                return;
            }

            static bool reported = false;
            if (!reported)
            {
                reported = true;

                std::array<uint8_t, sizeof(entry)> raw{};
                std::memcpy(raw.data(), &entry, raw.size());

                std::string hex{};
                for (const auto byte : raw)
                {
                    std::array<char, 4> pair{};
                    std::snprintf(pair.data(), pair.size(), "%02x ", byte);
                    hex += pair.data();
                }

                c.emu_ref.log.warn("kevent changelist entry %d of %d at 0x%" PRIx64 " decodes to filter %d, which is not a filter: %s\n",
                                   index, nchanges, changelist, entry.filter, hex.c_str());
            }
        }

        bool read_changelist(const macos_syscall_context& c, const uint64_t changelist, const int32_t nchanges,
                             std::vector<kevent_registration>& changes)
        {
            for (int32_t i = 0; i < nchanges; ++i)
            {
                macos_kevent_qos_entry entry{};
                if (!c.emu_ref.memory.try_read_memory(changelist + static_cast<uint64_t>(i) * sizeof(entry), &entry, sizeof(entry)))
                {
                    return false;
                }

                report_undecodable_entry(c, entry, i, changelist, nchanges);
                report_unmodelled_filter_once(c, entry.filter);

                kevent_registration change{.filter = entry.filter,
                                           .ident = entry.ident,
                                           .flags = entry.flags,
                                           .fflags = entry.fflags,
                                           .data = entry.data,
                                           .udata = entry.udata,
                                           .qos = entry.qos,
                                           .xflags = entry.xflags};
                std::copy(std::begin(entry.ext), std::end(entry.ext), std::begin(change.ext));
                changes.push_back(change);
            }

            return true;
        }

        // The answer of kevent_qos/kevent_id is the count of events actually placed in the caller's list.
        //
        // A workq or workloop queue never places any: its events belong to the workqueue threads the
        // kernel dispatches them to, not to whoever happens to be calling. Measured 2026-08-28 on the
        // host -- libdispatch's own KEVENT_FLAG_WORKQ call that triggers the manager knote passes a
        // 16-entry eventlist, is answered with 0, and the event surfaces on a worker instead. Handing
        // it to the caller here consumed the wake the worker was waiting for.
        bool deliver_events(const macos_syscall_context& c, const uint32_t kq, const uint64_t eventlist, const int32_t nevents,
                            int64_t& placed)
        {
            placed = 0;
            if (nevents <= 0 || eventlist == 0)
            {
                return true;
            }

            const auto* queue = c.proc.kqueues.find(kq);
            if (queue == nullptr || macos_kqueue_table::is_worker_queue(kq, *queue))
            {
                return true;
            }

            // nevents is guest-controlled (up to INT32_MAX), so the buffer is sized by what delivery
            // can actually produce; an empty queue places nothing and never touches eventlist.
            const auto available = std::min(static_cast<size_t>(nevents), c.proc.kqueues.pending_count(kq));
            if (available == 0)
            {
                return true;
            }

            std::vector<kevent_registration> events(available);
            const auto count = c.proc.kqueues.deliver(kq, events.data(), events.size());

            for (size_t i = 0; i < count; ++i)
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

                if (!c.emu_ref.memory.try_write_memory(eventlist + i * sizeof(entry), &entry, sizeof(entry)))
                {
                    return false;
                }

                placed = static_cast<int64_t>(i) + 1;
            }

            return true;
        }

        bool read_call(const macos_syscall_context& c, int32_t& nchanges, int32_t& nevents, uint64_t& changelist, uint64_t& eventlist,
                       uint32_t& call_flags, std::vector<kevent_registration>& changes)
        {
            changelist = get_macos_syscall_argument(c, 1);
            nchanges = static_cast<int32_t>(get_macos_syscall_argument(c, 2));
            eventlist = get_macos_syscall_argument(c, 3);
            nevents = static_cast<int32_t>(get_macos_syscall_argument(c, 4));
            call_flags = static_cast<uint32_t>(get_macos_syscall_argument(c, 7));

            if (nchanges < 0 || nevents < 0)
            {
                write_macos_syscall_error(c, MACOS_EINVAL);
                return false;
            }

            if (!read_changelist(c, changelist, nchanges, changes))
            {
                write_macos_syscall_error(c, MACOS_EFAULT);
                return false;
            }

            return true;
        }
    }

    void apply_worker_return_changelist(const macos_syscall_context& c, const uint64_t changelist, const int32_t nchanges,
                                        const bool workloop)
    {
        if (changelist == 0 || nchanges <= 0)
        {
            return;
        }

        std::vector<kevent_registration> changes{};
        if (!read_changelist(c, changelist, nchanges, changes))
        {
            return;
        }

        auto kq = uint64_t{MACOS_PROCESS_WORKQ_ID};
        if (workloop)
        {
            uint64_t workloop_id = 0;
            if (!c.emu_ref.memory.try_read_memory(changelist - sizeof(workloop_id), &workloop_id, sizeof(workloop_id)) || workloop_id == 0)
            {
                return;
            }

            kq = workloop_id;
            c.proc.kqueues.ensure(kq).is_workloop = true;
        }
        else
        {
            c.proc.kqueues.ensure(kq);
        }

        if (changelist_names_a_unit_twice(changes))
        {
            return;
        }

        const auto clock = sample_timer_clock(c);

        size_t user_events = 0;
        c.proc.kqueues.apply_changes(kq, changes.data(), changes.size(), &user_events, clock);
        fire_machport_knotes(c, changes);
        fire_due_timers(c, clock);
    }

    void sys_kqueue(const macos_syscall_context& c)
    {
        write_macos_syscall_result(c, static_cast<int64_t>(c.proc.kqueues.create()));
    }

    void sys_kevent_qos(const macos_syscall_context& c)
    {
        const auto kq_argument = get_macos_syscall_argument(c, 0);

        int32_t nchanges{};
        int32_t nevents{};
        uint64_t changelist{};
        uint64_t eventlist{};
        uint32_t call_flags{};
        std::vector<kevent_registration> changes{};
        if (!read_call(c, nchanges, nevents, changelist, eventlist, call_flags, changes))
        {
            return;
        }

        // Measured 2026-08-27: with KEVENT_FLAG_WORKQ the registration targets the process workqueue
        // and the kernel ignores the kq argument (libdispatch passes 0xffffffff), so an fd lookup
        // here would EBADF a call the kernel accepts.
        const auto workq = (call_flags & MACOS_KEVENT_FLAG_WORKQ) != 0;

        auto kq = MACOS_PROCESS_WORKQ_ID;
        if (!workq)
        {
            kq = static_cast<uint32_t>(kq_argument);
            const auto* entry = c.proc.fds.get(static_cast<int>(kq));
            if (entry == nullptr || entry->type != fd_type::kqueue)
            {
                write_macos_syscall_error(c, MACOS_EBADF);
                return;
            }
        }

        if (changelist_names_a_unit_twice(changes))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        c.proc.kqueues.ensure(kq);

        const auto clock = sample_timer_clock(c);

        size_t user_events = 0;
        c.proc.kqueues.apply_changes(kq, changes.data(), changes.size(), &user_events, clock);
        fire_machport_knotes(c, changes);
        fire_due_timers(c, clock);

        if (user_events != 0)
        {
            c.emu_ref.workqueue.wake_parked_worker(c.emu_ref);
        }

        // No changelist entry carries a dedicated thread-request bit on the plain workq; in xnu the
        // request is derived kernel-side from the knote registration, so the call itself is the request.
        if (workq)
        {
            c.proc.kqueues.record_workq_request(kq, changes.empty() ? kevent_registration{} : changes.front());

            if (const auto request = c.proc.kqueues.take_workq_request(kq))
            {
                c.emu_ref.workqueue.spawn_worker(c.emu_ref, *request, false);
            }
        }

        int64_t placed{};
        if (!deliver_events(c, kq, eventlist, nevents, placed))
        {
            write_macos_syscall_error(c, MACOS_EFAULT);
            return;
        }

        write_macos_syscall_result(c, placed);
    }

    void sys_kevent_id(const macos_syscall_context& c)
    {
        int32_t nchanges{};
        int32_t nevents{};
        uint64_t changelist{};
        uint64_t eventlist{};
        uint32_t call_flags{};
        std::vector<kevent_registration> changes{};
        if (!read_call(c, nchanges, nevents, changelist, eventlist, call_flags, changes))
        {
            return;
        }

        // Measured 2026-08-27: with KEVENT_FLAG_WORKLOOP, arg 0 is the workloop's dynamic kq id
        // (observed 0x300518520), not a file descriptor -- kevent_id and kevent_qos deliberately do
        // not share a shape. The id keys a real queue: libdispatch registers the connection's
        // EVFILT_MACHPORT knotes through kevent_id, so the changelist is stored like anywhere else.
        if ((call_flags & MACOS_KEVENT_FLAG_WORKLOOP) == 0)
        {
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                c.emu_ref.log.warn("kevent_id without KEVENT_FLAG_WORKLOOP names a dynamic kqueue id sogen does not model\n");
            }

            write_macos_syscall_error(c, MACOS_EBADF);
            return;
        }

        if (changelist_names_a_unit_twice(changes))
        {
            write_macos_syscall_error(c, MACOS_EINVAL);
            return;
        }

        const auto workloop_id = get_macos_syscall_argument(c, 0);
        c.proc.kqueues.ensure(workloop_id).is_workloop = true;

        const auto clock = sample_timer_clock(c);

        size_t user_events = 0;
        c.proc.kqueues.apply_changes(workloop_id, changes.data(), changes.size(), &user_events, clock);
        fire_machport_knotes(c, changes);
        fire_due_timers(c, clock);

        if (user_events != 0)
        {
            c.emu_ref.workqueue.wake_parked_worker(c.emu_ref);
        }

        for (const auto& change : changes)
        {
            if (change.filter == MACOS_EVFILT_WORKLOOP && (change.fflags & MACOS_NOTE_WL_THREAD_REQUEST) != 0)
            {
                c.proc.kqueues.record_workq_request(MACOS_PROCESS_WORKQ_ID, change);
            }
        }

        // A recorded thread request is answered with a worker, consume-once so one request spawns one
        // thread and a burst of registrations spawns a bounded pool rather than a loop.
        if (const auto request = c.proc.kqueues.take_workq_request(MACOS_PROCESS_WORKQ_ID))
        {
            c.emu_ref.workqueue.spawn_worker(c.emu_ref, *request, true);
        }

        write_macos_syscall_result(c, 0);
    }

}
