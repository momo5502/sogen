#pragma once

#include "std_include.hpp"
#include "macos_platform.hpp"

#include <guest/guest_fd_table.hpp>

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace sogen
{

    struct kevent_registration
    {
        int16_t filter{};
        uint64_t ident{};
        uint16_t flags{};
        uint32_t fflags{};
        int64_t data{};
        uint64_t udata{};
        int32_t qos{};
        uint32_t xflags{};
        uint64_t ext[4]{};
    };

    // The clock an EVFILT_TIMER registration is resolved against. The table cannot read the guest's
    // counter, so the caller samples it: `now` and every deadline the table stores are CNTVCT_EL0
    // ticks, the one clock __semwait_signal, the psynch condition waits and mk_timer already share.
    // `calendar_ns` is the wall clock a NOTE_ABSOLUTE registration without NOTE_MACHTIME counts from.
    struct kevent_timer_clock
    {
        uint64_t now{};
        uint32_t timebase_numer{1};
        uint32_t timebase_denom{1};
        uint64_t calendar_ns{};
    };

    struct kevent_timer
    {
        uint64_t deadline{};
        uint64_t interval{};
        uint64_t fired{};
        bool repeating{};
        bool armed{};
        bool enabled{true};
    };

    // Every value below is guest-controlled and the unit multiplies it, so an unclamped conversion
    // wraps a nonsense interval into a deadline in the past -- a timer that then fires without end.
    inline uint64_t kevent_saturating_mul(const uint64_t value, const uint64_t multiplier)
    {
        constexpr auto limit = std::numeric_limits<uint64_t>::max();
        return multiplier != 0 && value > limit / multiplier ? limit : value * multiplier;
    }

    inline uint64_t kevent_saturating_add(const uint64_t value, const uint64_t addend)
    {
        constexpr auto limit = std::numeric_limits<uint64_t>::max();
        return value > limit - addend ? limit : value + addend;
    }

    inline uint64_t kevent_nanoseconds_to_ticks(const kevent_timer_clock& clock, const uint64_t nanoseconds)
    {
        const uint64_t numer = clock.timebase_numer != 0 ? clock.timebase_numer : 1;
        const uint64_t denom = clock.timebase_denom;

        return kevent_saturating_add(kevent_saturating_mul(nanoseconds / numer, denom), (nanoseconds % numer) * denom / numer);
    }

    // The unit bits are mutually exclusive and say what the changelist's `data` counts; more than one
    // is EINVAL and none of them is milliseconds (xnu bsd/kern/kern_event.c filt_timervalidate,
    // measured 2026-08-28: data=150 with no unit bit fired after 151.7 ms). Zero means the value is
    // already in mach ticks.
    inline std::optional<uint64_t> kevent_timer_unit_nanoseconds(const uint32_t fflags)
    {
        switch (fflags & (MACOS_NOTE_SECONDS | MACOS_NOTE_USECONDS | MACOS_NOTE_NSECONDS | MACOS_NOTE_MACHTIME))
        {
        case MACOS_NOTE_SECONDS:
            return 1000000000ULL;
        case MACOS_NOTE_USECONDS:
            return 1000ULL;
        case MACOS_NOTE_NSECONDS:
            return 1ULL;
        case MACOS_NOTE_MACHTIME:
            return 0ULL;
        case 0:
            return 1000000ULL;
        default:
            return std::nullopt;
        }
    }

    // Without NOTE_ABSOLUTE the value is an interval the knote repeats on; with it the value is a
    // one-shot deadline, read against mach absolute time when NOTE_MACHTIME is set and against the
    // calendar clock when it is not. Measured 2026-08-28 on the host: a NOTE_ABSOLUTE|NOTE_NSECONDS
    // deadline built from mach_absolute_time fired instantly (it is decades in the past on that
    // scale), the same deadline built from gettimeofday fired after the 140 ms it named.
    inline std::optional<kevent_timer> resolve_kevent_timer(const kevent_registration& change, const kevent_timer_clock& clock)
    {
        const auto unit = kevent_timer_unit_nanoseconds(change.fflags);
        if (!unit.has_value())
        {
            return std::nullopt;
        }

        const auto value = static_cast<uint64_t>(std::max<int64_t>(change.data, 0));

        if ((change.fflags & MACOS_NOTE_ABSOLUTE) == 0)
        {
            const auto interval = *unit == 0 ? value : kevent_nanoseconds_to_ticks(clock, kevent_saturating_mul(value, *unit));
            return kevent_timer{
                .deadline = kevent_saturating_add(clock.now, interval), .interval = interval, .repeating = true, .armed = true};
        }

        if (*unit == 0)
        {
            return kevent_timer{.deadline = value, .armed = true};
        }

        const auto target_ns = kevent_saturating_mul(value, *unit);
        const auto remaining_ns = target_ns > clock.calendar_ns ? target_ns - clock.calendar_ns : 0;
        return kevent_timer{.deadline = kevent_saturating_add(clock.now, kevent_nanoseconds_to_ticks(clock, remaining_ns)), .armed = true};
    }

    // Layout of struct kevent_qos_s from xnu bsd/sys/event_private.h, which is the only place it is
    // declared; the SDK's sys/event.h names kevent_qos but not the struct. Measured 2026-08-27 against
    // this build's libdispatch; it is NOT kevent64_s plus trailing fields.
    struct macos_kevent_qos_entry
    {
        uint64_t ident;
        int16_t filter;
        uint16_t flags;
        int32_t qos;
        uint64_t udata;
        uint32_t fflags;
        uint32_t xflags;
        int64_t data;
        uint64_t ext[4];
    };

    static_assert(sizeof(macos_kevent_qos_entry) == 72);

    // libdispatch passes 0xffffffff as the kq of a KEVENT_FLAG_WORKQ call because the kernel ignores
    // the argument there, so the process workqueue is keyed by that value rather than by an invented
    // sentinel. It collides with no descriptor: kqueue() hands out real fds.
    constexpr uint32_t MACOS_PROCESS_WORKQ_ID = 0xFFFFFFFFu;

    struct macos_kqueue
    {
        std::vector<kevent_registration> registrations{};
        std::deque<kevent_registration> pending{};
        std::optional<kevent_registration> workq_request{};
        // Keyed by ident, because a knote is named by (filter, ident) and every entry here is an
        // EVFILT_TIMER one.
        std::map<uint64_t, kevent_timer> timers{};
        // kevent_id keys its queue by the workloop's dynamic kq id (a guest pointer) instead of a
        // descriptor; events on such a queue are delivered to workqueue workers, not kevent callers.
        bool is_workloop{};
    };

    struct macos_syscall_context;

    // WQOPS_THREAD_KEVENT_RETURN and WQOPS_THREAD_WORKLOOP_RETURN carry a changelist of their own: a
    // worker that has handled its events registers the next round of knotes in the same call that parks
    // it, and the call never returns to userspace to be answered. Measured 2026-08-28 on the host: every
    // libdispatch timer -- dispatch_after and a repeating dispatch_source alike -- is armed exactly this
    // way and never through kevent_qos, so a workqueue return that drops its changelist drops every
    // timer the process owns. `workloop` picks where the changes land, and the workloop's kq id is read
    // from the word below the buffer, which is where the worker was handed it.
    void apply_worker_return_changelist(const macos_syscall_context& c, uint64_t changelist, int32_t nchanges, bool workloop);

    class macos_kqueue_table
    {
      public:
        explicit macos_kqueue_table(guest_fd_table& fds)
            : fds_(fds)
        {
        }

        // kqueue() returns a real descriptor on macOS -- close() works on it and it occupies fd
        // space -- so the id comes from the fd table and the object is keyed by it. Workloop ids are
        // guest pointers, so the key is 64-bit even though descriptors are not.
        uint32_t create()
        {
            guest_fd entry{};
            entry.type = fd_type::kqueue;
            const auto fd = this->fds_.allocate(std::move(entry));
            this->queues_.emplace(static_cast<uint32_t>(fd), macos_kqueue{});
            return static_cast<uint32_t>(fd);
        }

        void destroy(const uint32_t kq)
        {
            this->queues_.erase(kq);
        }

        macos_kqueue* find(const uint64_t kq)
        {
            const auto it = this->queues_.find(kq);
            return it == this->queues_.end() ? nullptr : &it->second;
        }

        const macos_kqueue* find(const uint64_t kq) const
        {
            const auto it = this->queues_.find(kq);
            return it == this->queues_.end() ? nullptr : &it->second;
        }

        macos_kqueue& ensure(const uint64_t kq)
        {
            return this->queues_[kq];
        }

        // `fired` counts the events the changelist itself produced. EVFILT_USER has no kernel event
        // source: NOTE_TRIGGER in a changelist entry is the whole event, so applying a change and
        // delivering one are the same step for that filter.
        bool apply_changes(const uint64_t kq, const kevent_registration* changes, const size_t count, size_t* fired = nullptr,
                           const kevent_timer_clock& clock = {})
        {
            auto* queue = this->find(kq);
            if (queue == nullptr || (changes == nullptr && count != 0))
            {
                return false;
            }

            for (size_t i = 0; i < count; ++i)
            {
                const auto& change = changes[i];
                const auto same_knote = [&change](const kevent_registration& registration) {
                    return registration.filter == change.filter && registration.ident == change.ident;
                };

                if ((change.flags & MACOS_EV_DELETE) != 0)
                {
                    std::erase_if(queue->registrations, same_knote);
                    std::erase_if(queue->pending, same_knote);
                    if (change.filter == MACOS_EVFILT_TIMER)
                    {
                        queue->timers.erase(change.ident);
                    }

                    continue;
                }

                auto existing = std::ranges::find_if(queue->registrations, same_knote);
                if (change.filter == MACOS_EVFILT_TIMER)
                {
                    apply_timer_change(*queue, existing, change, clock);
                    continue;
                }

                if (change.filter == MACOS_EVFILT_USER)
                {
                    const auto adding = (change.flags & MACOS_EV_ADD) != 0;
                    if (existing == queue->registrations.end() && !adding)
                    {
                        continue;
                    }

                    auto& knote = adding || existing == queue->registrations.end()
                                      ? *this->add_or_replace_user_knote(*queue, existing, change)
                                      : *existing;

                    // filt_usertouch (xnu bsd/kern/kern_event.c) updates only the stored flag set and
                    // the stored data. A trigger arrives as a changelist entry with no EV_ADD and an
                    // otherwise empty struct, so copying its flags over the knote's would drop the
                    // EV_CLEAR the registration asked for and turn a one-shot wake into a level-
                    // triggered one.
                    knote.data = change.data;
                    knote.fflags = combine_user_fflags(knote.fflags, change.fflags);

                    if ((change.fflags & MACOS_NOTE_TRIGGER) != 0 && trigger_user_knote(*queue, knote) && fired != nullptr)
                    {
                        ++*fired;
                    }

                    continue;
                }

                if (existing != queue->registrations.end())
                {
                    *existing = change;
                }
                else
                {
                    queue->registrations.push_back(change);
                }
            }

            return true;
        }

        // filt_usertouch (xnu bsd/kern/kern_event.c): the control bits pick how the changelist's user
        // flags fold into the knote's stored set, and the delivered event carries the stored set, not
        // the changelist's.
        static uint32_t combine_user_fflags(const uint32_t stored, const uint32_t change)
        {
            const auto flags = change & MACOS_NOTE_FFLAGSMASK;

            switch (change & MACOS_NOTE_FFCTRLMASK)
            {
            case MACOS_NOTE_FFAND:
                return stored & flags;
            case MACOS_NOTE_FFOR:
                return stored | flags;
            case MACOS_NOTE_FFCOPY:
                return flags;
            default:
                return stored;
            }
        }

        size_t pending_count(const uint64_t kq) const
        {
            const auto* queue = this->find(kq);
            return queue == nullptr ? 0 : queue->pending.size();
        }

        size_t deliver(const uint64_t kq, kevent_registration* out, const size_t max)
        {
            auto* queue = this->find(kq);
            if (queue == nullptr || out == nullptr)
            {
                return 0;
            }

            const auto count = std::min(max, queue->pending.size());
            for (size_t i = 0; i < count; ++i)
            {
                out[i] = queue->pending.front();
                queue->pending.pop_front();
                retire_delivered(*queue, out[i]);
            }

            return count;
        }

        // The EVFILT_MACHPORT event source: a message arriving on a knote-monitored port fires the
        // knote. Measured 2026-08-27 on the host (cgsdemo under lldb): the delivered event is the
        // registration with EV_VANISHED cleared, fflags zeroed, the port name in data, udata and ext
        // echoed. A knote that is already pending does not queue a second event.
        size_t note_port_message(const uint32_t port)
        {
            size_t produced = 0;
            for (auto& [id, queue] : this->queues_)
            {
                for (const auto& registration : queue.registrations)
                {
                    if (registration.filter != MACOS_EVFILT_MACHPORT || registration.ident != port)
                    {
                        continue;
                    }

                    const auto already_pending = std::ranges::find_if(queue.pending, [&port](const kevent_registration& event) {
                        return event.filter == MACOS_EVFILT_MACHPORT && event.ident == port;
                    });
                    if (already_pending != queue.pending.end())
                    {
                        continue;
                    }

                    auto event = registration;
                    event.flags &= ~MACOS_EV_VANISHED;
                    event.fflags = 0;
                    event.data = port;
                    queue.pending.push_back(event);
                    ++produced;
                }
            }

            return produced;
        }

        static bool is_worker_queue(const uint64_t id, const macos_kqueue& queue)
        {
            return queue.is_workloop || id == MACOS_PROCESS_WORKQ_ID;
        }

        bool has_workq_events() const
        {
            for (const auto& [id, queue] : this->queues_)
            {
                if (is_worker_queue(id, queue) && !queue.pending.empty())
                {
                    return true;
                }
            }

            return false;
        }

        // The workq/workloop queues are drained one at a time, the way the measured wakes each carried
        // one queue's events; the caller re-checks after the worker parks again. The queue id goes to
        // the worker too: __pthread_wqthread reads the workloop's kq id from the word just below the
        // kevent buffer (measured 2026-08-27 on the host).
        size_t drain_workq_events(kevent_registration* out, const size_t max, uint64_t& queue_id, bool& from_workloop)
        {
            for (auto& [id, queue] : this->queues_)
            {
                if (!is_worker_queue(id, queue) || queue.pending.empty())
                {
                    continue;
                }

                queue_id = id;
                from_workloop = queue.is_workloop;
                const auto count = std::min(max, queue.pending.size());
                for (size_t i = 0; i < count; ++i)
                {
                    out[i] = queue.pending.front();
                    queue.pending.pop_front();
                    retire_delivered(queue, out[i]);
                }

                return count;
            }

            return 0;
        }

        bool has_workq_request(const uint64_t kq) const
        {
            const auto* queue = this->find(kq);
            return queue != nullptr && queue->workq_request.has_value();
        }

        // Consume-once: Task 4 spawns one worker per request, so taking clears it.
        std::optional<kevent_registration> take_workq_request(const uint64_t kq)
        {
            auto* queue = this->find(kq);
            if (queue == nullptr)
            {
                return std::nullopt;
            }

            auto request = std::move(queue->workq_request);
            queue->workq_request.reset();
            return request;
        }

        // The only event source in the table that no guest action drives. The scheduler owns the answer
        // to "nothing can run": it asks for the earliest deadline, lets the guest's counter reach it and
        // fires it, the same step it already takes for a timed park and an armed mk_timer.
        std::optional<uint64_t> earliest_timer_deadline() const
        {
            std::optional<uint64_t> earliest{};

            for (const auto& [id, queue] : this->queues_)
            {
                for (const auto& [ident, timer] : queue.timers)
                {
                    if (!timer.armed || !timer.enabled)
                    {
                        continue;
                    }

                    if (!earliest.has_value() || timer.deadline < *earliest)
                    {
                        earliest = timer.deadline;
                    }
                }
            }

            return earliest;
        }

        // Measured 2026-08-28 on the host: a 100 ms interval knote left unread for 350 ms delivered one
        // event carrying data=3, so an expiry that nobody has collected yet raises the count of the
        // event already pending rather than queueing a second one. The answer is the number of knotes
        // that newly became pending, which is what decides whether a worker has to be woken.
        size_t fire_due_timers(const uint64_t now)
        {
            size_t produced = 0;

            for (auto& [id, queue] : this->queues_)
            {
                for (auto& [ident, timer] : queue.timers)
                {
                    if (!timer.armed || !timer.enabled || timer.deadline > now)
                    {
                        continue;
                    }

                    const auto expirations = timer.interval != 0 ? 1 + (now - timer.deadline) / timer.interval : 1;
                    timer.fired += expirations;

                    if (timer.repeating)
                    {
                        timer.deadline += expirations * timer.interval;
                    }
                    else
                    {
                        timer.armed = false;
                    }

                    if (raise_timer_event(queue, ident, timer.fired))
                    {
                        ++produced;
                    }
                }
            }

            return produced;
        }

        void record_workq_request(const uint64_t kq, const kevent_registration& request)
        {
            auto& queue = this->ensure(kq);
            if (!queue.workq_request.has_value())
            {
                queue.workq_request = request;
            }
        }

      private:
        // filt_timerattach (xnu bsd/kern/kern_event.c) rewrites the knote's flags before anything is
        // delivered: a timer is always EV_CLEAR, and NOTE_ABSOLUTE makes it EV_ONESHOT whether the
        // registration asked for one or not. Measured 2026-08-28: a NOTE_ABSOLUTE|NOTE_MACHTIME knote
        // added with EV_ADD|EV_ENABLE (0x5) came back as 0x35 and never fired a second time.
        static kevent_registration attached_timer_knote(const kevent_registration& change)
        {
            auto knote = change;
            knote.flags |= MACOS_EV_CLEAR;

            if ((change.fflags & MACOS_NOTE_ABSOLUTE) != 0)
            {
                knote.flags |= MACOS_EV_ONESHOT;
            }

            return knote;
        }

        static void apply_timer_change(macos_kqueue& queue, const std::vector<kevent_registration>::iterator existing,
                                       const kevent_registration& change, const kevent_timer_clock& clock)
        {
            if ((change.flags & MACOS_EV_ADD) != 0)
            {
                const auto knote = attached_timer_knote(change);
                if (existing != queue.registrations.end())
                {
                    *existing = knote;
                }
                else
                {
                    queue.registrations.push_back(knote);
                }

                queue.timers[change.ident] = resolve_kevent_timer(change, clock).value_or(kevent_timer{});
                return;
            }

            const auto timer = queue.timers.find(change.ident);
            if (timer == queue.timers.end())
            {
                return;
            }

            if ((change.flags & MACOS_EV_DISABLE) != 0)
            {
                timer->second.enabled = false;
                return;
            }

            // Measured 2026-08-28: an interval knote disabled across three of its intervals and then
            // re-enabled fired at once with data=1, not with the count of what it slept through, so the
            // re-enable re-phases the deadline instead of catching up.
            if ((change.flags & MACOS_EV_ENABLE) != 0)
            {
                timer->second.enabled = true;
                if (timer->second.armed && timer->second.deadline < clock.now)
                {
                    timer->second.deadline = clock.now;
                }
            }
        }

        // Measured 2026-08-28: the delivered event is the attached knote with its fflags zeroed and
        // `data` set to how many expirations the guest has not collected yet.
        static bool raise_timer_event(macos_kqueue& queue, const uint64_t ident, const uint64_t fired)
        {
            const auto knote = std::ranges::find_if(queue.registrations, [ident](const kevent_registration& registration) {
                return registration.filter == MACOS_EVFILT_TIMER && registration.ident == ident;
            });

            if (knote == queue.registrations.end())
            {
                return false;
            }

            auto event = *knote;
            event.fflags = 0;
            event.data = static_cast<int64_t>(fired);

            const auto already_pending = std::ranges::find_if(queue.pending, [ident](const kevent_registration& pending) {
                return pending.filter == MACOS_EVFILT_TIMER && pending.ident == ident;
            });

            if (already_pending != queue.pending.end())
            {
                *already_pending = event;
                return false;
            }

            queue.pending.push_back(event);
            return true;
        }

        // What happens to a knote once its event has been handed to the guest. EV_ONESHOT deletes it;
        // EV_CLEAR and EV_DISPATCH deactivate it, which popping the event already did; a knote with
        // none of the three stays active, and xnu re-delivers it on the next drain.
        static void retire_delivered(macos_kqueue& queue, const kevent_registration& event)
        {
            const auto same_knote = [&event](const kevent_registration& registration) {
                return registration.filter == event.filter && registration.ident == event.ident;
            };

            const auto oneshot = (event.flags & MACOS_EV_ONESHOT) != 0;

            if (event.filter == MACOS_EVFILT_TIMER)
            {
                if (oneshot)
                {
                    queue.timers.erase(event.ident);
                }
                else if (const auto timer = queue.timers.find(event.ident); timer != queue.timers.end())
                {
                    timer->second.fired = 0;
                }
            }

            if (oneshot)
            {
                std::erase_if(queue.registrations, same_knote);
                return;
            }

            if (event.filter != MACOS_EVFILT_USER || (event.flags & (MACOS_EV_CLEAR | MACOS_EV_DISPATCH)) != 0)
            {
                return;
            }

            const auto knote = std::ranges::find_if(queue.registrations, same_knote);
            if (knote != queue.registrations.end())
            {
                queue.pending.push_back(*knote);
            }
        }

        static kevent_registration* add_or_replace_user_knote(macos_kqueue& queue,
                                                              const std::vector<kevent_registration>::iterator existing,
                                                              const kevent_registration& change)
        {
            if (existing != queue.registrations.end())
            {
                const auto carried = existing->fflags;
                *existing = change;
                existing->fflags = carried;
                return &*existing;
            }

            auto& added = queue.registrations.emplace_back(change);
            added.fflags = 0;
            return &added;
        }

        static bool trigger_user_knote(macos_kqueue& queue, const kevent_registration& knote)
        {
            const auto already_pending = std::ranges::find_if(queue.pending, [&knote](const kevent_registration& event) {
                return event.filter == MACOS_EVFILT_USER && event.ident == knote.ident;
            });

            if (already_pending != queue.pending.end())
            {
                *already_pending = knote;
                return false;
            }

            queue.pending.push_back(knote);
            return true;
        }

        guest_fd_table& fds_;
        std::map<uint64_t, macos_kqueue> queues_{};
    };

}
