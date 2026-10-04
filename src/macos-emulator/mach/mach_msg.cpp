#include "../std_include.hpp"
#include "mach_msg.hpp"

#include "../bsd_syscall_dispatcher.hpp"
#include "../macos_emulator.hpp"
#include "mig_kernel_servers.hpp"

#include <algorithm>
#include <ranges>
#include <set>

namespace sogen::mach
{
    namespace
    {
        bool destination_is_kernel_object(macos_emulator& emu, const port_name_t name)
        {
            return emu.mach.ports.object_of(name).kind != kernel_object_kind::none;
        }

        // MACH64_MSG_VECTOR replaces the trap's flat buffer with an array of mach_msg_vector_t and turns
        // both size halves of the trap into element counts: element 0 is the message, element 1 is
        // libsyscall's auxiliary data. Measured 2026-08-28 on the host, breaking on mach_msg2_internal in
        // an AppKit process: the header fields in the trap registers duplicate element 0's header, but
        // the msgh_size inside the buffer is frequently zero -- msgv_send_size is the authoritative one,
        // which is why libnotify's checkins looked like a family with a zero-size header.
        void resolve_msg_vector(macos_emulator& emu, msg_call& call)
        {
            const auto send_count = call.send_size;
            const auto rcv_count = call.rcv_size;
            const auto sending = (call.options & msg_option::send_msg) != 0;
            const auto receiving = (call.options & msg_option::rcv_msg) != 0;

            call.send_size = 0;
            call.rcv_size = 0;
            call.header.size = 0;

            if (sending && send_count == 0)
            {
                call.decode_error = msgr::send_msg_too_small;
                return;
            }

            if (sending && send_count > MSG_VECTOR_MAX_COUNT)
            {
                call.decode_error = msgr::send_invalid_data;
                return;
            }

            if (receiving && (rcv_count == 0 || rcv_count > MSG_VECTOR_MAX_COUNT))
            {
                call.decode_error = msgr::rcv_invalid_arguments;
                return;
            }

            const auto count = std::max(sending ? send_count : 0u, receiving ? rcv_count : 0u);
            if (count == 0)
            {
                return;
            }

            std::array<uint8_t, MSG_VECTOR_ELEMENT_SIZE * MSG_VECTOR_MAX_COUNT> elements{};
            if (!emu.memory.try_read_memory(call.buffer, elements.data(), static_cast<size_t>(count) * MSG_VECTOR_ELEMENT_SIZE))
            {
                call.decode_error = sending ? msgr::send_invalid_data : msgr::rcv_invalid_data;
                return;
            }

            const auto element = [&elements](const uint32_t index) {
                return read_msg_vector_element(
                    std::span<const uint8_t>{elements}.subspan(index * MSG_VECTOR_ELEMENT_SIZE, MSG_VECTOR_ELEMENT_SIZE));
            };

            const auto message = element(MSG_VECTOR_INDEX_MESSAGE);
            call.buffer = message.data;
            call.rcv_buffer = message.rcv_address != 0 ? message.rcv_address : message.data;
            call.send_size = sending ? message.send_size : 0;
            call.rcv_size = receiving ? message.rcv_size : 0;
            call.header.size = call.send_size;

            if (count > MSG_VECTOR_INDEX_AUX)
            {
                const auto aux = element(MSG_VECTOR_INDEX_AUX);
                call.aux_buffer = aux.rcv_address != 0 ? aux.rcv_address : aux.data;
                call.aux_send_size = sending && send_count > MSG_VECTOR_INDEX_AUX ? aux.send_size : 0;
                call.aux_rcv_size = receiving && rcv_count > MSG_VECTOR_INDEX_AUX ? aux.rcv_size : 0;
            }

            if (sending && call.send_size == 0)
            {
                call.decode_error = msgr::send_msg_too_small;
            }
        }

        // libsyscall attaches auxiliary data to nearly every message it sends -- an 8-byte
        // mach_msg_aux_header_t whose msgdh_size covers the whole element, then a tracing payload the
        // peer's libsyscall reads back. sogen carries the message and drops the aux, so the peer sees
        // the empty header deliver_reply writes rather than what the sender attached.
        void report_dropped_aux(macos_emulator& emu, const msg_call& call)
        {
            if (call.aux_send_size == 0)
            {
                return;
            }

            static std::set<int32_t> reported{};
            if (reported.insert(call.header.id).second)
            {
                emu.log.warn("mach_msg2 vector: routine 0x%x carries %u bytes of auxiliary data, which sogen does not forward\n",
                             call.header.id, call.aux_send_size);
            }
        }
    }

    // Clearing the park is the whole wake. The woken thread is not activated here, mid-send: the
    // cooperative scheduler hands it the CPU at the next reschedule point, and its rewound pc re-runs
    // the receive, which now finds the queue non-empty. A thread waiting on a port set is parked on
    // the set's name rather than the member's, so the sets have to be woken too.
    size_t wake_port_receivers(macos_emulator& emu, const port_name_t name)
    {
        size_t unparked = 0;

        if (const auto woken = emu.process.wake_receivers_of(name); woken != 0)
        {
            emu.log.info("waking thread %" PRIu64 " parked on port 0x%x\n", woken, name);
            ++unparked;
        }

        for (const auto set_name : emu.mach.ports.sets_containing(name))
        {
            if (const auto woken = emu.process.wake_receivers_of(set_name); woken != 0)
            {
                emu.log.info("waking thread %" PRIu64 " parked on port set 0x%x\n", woken, set_name);
                ++unparked;
            }
        }

        return unparked;
    }

    void announce_queued_message(macos_emulator& emu, const port_name_t name, const int32_t routine)
    {
        const auto unparked = wake_port_receivers(emu, name);

        // A knote can be registered on a port set rather than on the member the message landed on --
        // libdispatch does exactly that for a channel it has folded into a workloop -- so the sets have
        // to be notified too, the same way their waiters are woken.
        auto notes = emu.process.kqueues.note_port_message(name);
        for (const auto set_name : emu.mach.ports.sets_containing(name))
        {
            notes += emu.process.kqueues.note_port_message(set_name);
        }

        if (notes != 0)
        {
            emu.workqueue.wake_parked_worker(emu);
            return;
        }

        if (unparked != 0)
        {
            return;
        }

        // A message with no waiter and no knote is one the guest can only find by polling, and nothing
        // in a run loop polls. This is the difference between "sogen never answered" and "sogen answered
        // into a queue nobody is watching", and the two need opposite fixes.
        static std::set<std::pair<port_name_t, int32_t>> reported{};
        if (reported.emplace(name, routine).second)
        {
            emu.log.warn("routine 0x%x queued on port 0x%x with no waiter and no kqueue registration\n", routine, name);
        }
    }

    // xnu's mk_timer_expire sends a bare header with msgh_id zero: the receiver identifies the timer by
    // the port the message arrived on, which is what CFRunLoop reads out of msgh_local_port.
    void deliver_timer_expiration(macos_emulator& emu, const port_name_t name)
    {
        auto* entry = emu.mach.ports.destination_of(name);
        if (entry == nullptr)
        {
            return;
        }

        std::vector<uint8_t> message(MSG_HEADER_SIZE, 0);
        write_msg_header(message, msg_header{
                                      .bits = 0,
                                      .size = MSG_HEADER_SIZE,
                                      .remote_port = PORT_NULL,
                                      .local_port = name,
                                      .voucher_port = PORT_NULL,
                                      .id = 0,
                                  });

        entry->queue.push_back(std::move(message));
        announce_queued_message(emu, name, 0);
    }

    // A disposition names one thing when a message is sent and another once it has arrived. The
    // make- and copy- forms are instructions to the kernel about a right to create; a message the guest
    // is *receiving* can only carry the right itself, so it says move-. Handing back the send-side form
    // is not a cosmetic error: the guest's own message-teardown path reads the header disposition, sees
    // a make- form on an arriving message, and tries to extract a right from it -- which is another
    // message, with the same reply, forever.
    uint8_t received_disposition(const uint8_t sent)
    {
        switch (sent)
        {
        case disposition::make_send:
        case disposition::copy_send:
        case disposition::move_send:
            return disposition::move_send;

        case disposition::make_send_once:
        case disposition::move_send_once:
            return disposition::move_send_once;

        default:
            return sent;
        }
    }

    // These are the bits of a message as *received*, because that is what the guest finds in its buffer:
    // the port it arrived on is the local one, and a mig reply conveys no right back, so the remote slot
    // is empty. mig's generated client checks msgh_remote_port and returns MIG_TYPE_ERROR when it is not
    // null, which is what "BUG IN LIBPTHREAD: host_info() failed" turned out to be.
    uint32_t reply_bits_for(const uint32_t request_bits, const bool complex)
    {
        const auto bits = make_bits(0, received_disposition(local_disposition(request_bits)));
        return complex ? (bits | BITS_COMPLEX) : bits;
    }

    msg_reply make_mig_error_reply(const msg_call& call, const kern_return_t code)
    {
        std::vector<uint8_t> bytes(MIG_REPLY_ERROR_SIZE, 0);

        write_msg_header(bytes, {
                                    .bits = reply_bits_for(call.header.bits, false),
                                    .size = static_cast<uint32_t>(MIG_REPLY_ERROR_SIZE),
                                    .remote_port = PORT_NULL,
                                    .local_port = call.header.local_port,
                                    .voucher_port = 0,
                                    .id = call.header.id + 100,
                                });

        std::ranges::copy(NDR_RECORD, bytes.begin() + MSG_HEADER_SIZE);
        write_u32(bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE, static_cast<uint32_t>(code));

        return {.bytes = std::move(bytes), .valid = true};
    }

    msg_call decode_msg2_call(const macos_syscall_context& c)
    {
        const auto a2 = get_macos_syscall_argument(c, 2);
        const auto a3 = get_macos_syscall_argument(c, 3);
        const auto a4 = get_macos_syscall_argument(c, 4);
        const auto a5 = get_macos_syscall_argument(c, 5);
        const auto a6 = get_macos_syscall_argument(c, 6);

        msg_call call{
            .buffer = get_macos_syscall_argument(c, 0),
            .options = get_macos_syscall_argument(c, 1),
            .header =
                {
                    .bits = static_cast<uint32_t>(a2),
                    .size = static_cast<uint32_t>(a2 >> 32),
                    .remote_port = static_cast<uint32_t>(a3),
                    .local_port = static_cast<uint32_t>(a3 >> 32),
                    .voucher_port = static_cast<uint32_t>(a4),
                    .id = static_cast<int32_t>(static_cast<uint32_t>(a4 >> 32)),
                },
            .send_size = static_cast<uint32_t>(a2 >> 32),
            .descriptor_count = static_cast<uint32_t>(a5),
            .rcv_name = static_cast<uint32_t>(a5 >> 32),
            .rcv_size = static_cast<uint32_t>(a6),
            .priority = static_cast<uint32_t>(a6 >> 32),
            .timeout = get_macos_syscall_argument(c, 7),
        };

        if ((call.options & msg_option::vector) != 0)
        {
            resolve_msg_vector(c.emu_ref, call);
        }

        return call;
    }

    std::vector<uint8_t> read_message_body(macos_emulator& emu, const msg_call& call)
    {
        if (call.send_size < MSG_HEADER_SIZE)
        {
            return {};
        }

        const auto body_size = static_cast<size_t>(call.send_size) - MSG_HEADER_SIZE;
        std::vector<uint8_t> body(body_size, 0);

        if (body_size != 0 && !emu.memory.try_read_memory(call.buffer + MSG_HEADER_SIZE, body.data(), body_size))
        {
            return {};
        }

        return body;
    }

    namespace
    {
        // The trailer a receive asked for, per the elements field of the receive options
        // (MACH_RCV_TRAILER_ELEMENTS, bits 24-27). An audit trailer is what proves to libxpc that a
        // reply really came from launchd; delivering only the 8-byte null trailer when an audit trailer
        // was requested makes a service-port reply indistinguishable from a spoof.
        std::vector<uint8_t> make_requested_trailer(const uint64_t options)
        {
            const auto elements = (options >> 24) & 0xf;

            size_t size = 8;
            if (elements == 1)
            {
                size = 16; // mach_msg_seqno_trailer_t
            }
            else if (elements == 2)
            {
                size = 24; // mach_msg_security_trailer_t
            }
            else if (elements == 3)
            {
                size = 52; // mach_msg_audit_trailer_t
            }
            else if (elements >= 4)
            {
                size = 68; // mach_msg_mac_trailer_t
            }

            std::vector<uint8_t> trailer(size, 0);
            write_u32(trailer, 0, 0);
            write_u32(trailer, 4, static_cast<uint32_t>(size));

            if (elements >= 3)
            {
                // The audit token of the daemon sogen stands in for: root/wheel, and pid 1 because the
                // replies impersonate launchd. Measured token of a real launchd lookup reply:
                // auid -1, euid/egid/ruid 0, rgid 1.
                write_u32(trailer, 20, 0xffffffffu);
                write_u32(trailer, 36, 1);
                write_u32(trailer, 40, 1);
                write_u32(trailer, 48, 1);
            }

            return trailer;
        }
    }

    namespace
    {
        // A vector receive answers the aux element too. Measured 2026-08-28 by poisoning the aux buffer
        // of a live CFRunLoop receive: on success xnu writes a zeroed mach_msg_aux_header_t there when
        // the delivered message carries no auxiliary data, and leaves the buffer alone when the receive
        // fails. sogen never produces aux data, so zero is the honest answer -- and without writing it
        // the guest reads back whatever the buffer held, which for a send-and-receive is the aux it
        // just sent.
        mach_msg_return_t deliver_empty_aux(macos_emulator& emu, const msg_call& call)
        {
            if (call.aux_rcv_size < MSG_AUX_HEADER_SIZE)
            {
                return msgr::success;
            }

            constexpr std::array<uint8_t, MSG_AUX_HEADER_SIZE> header{};
            if (!emu.memory.try_write_memory(call.aux_buffer, header.data(), header.size()))
            {
                return msgr::rcv_invalid_data;
            }

            return msgr::success;
        }
    }

    mach_msg_return_t deliver_reply(macos_emulator& emu, const msg_call& call, const std::span<const uint8_t> reply)
    {
        const auto trailer = make_requested_trailer(call.options);
        const auto target = call.rcv_buffer != 0 ? call.rcv_buffer : call.buffer;

        if (reply.size() > call.rcv_size || trailer.size() > call.rcv_size - reply.size())
        {
            // MACH_RCV_TOO_LARGE leaves the required size in the receive buffer's header, which is what
            // the caller reallocates against before receiving again: libxpc's _xpc_pipe_try_receive
            // reads msgh_size out of the buffer it just passed.
            if (call.rcv_size >= MSG_HEADER_SIZE && reply.size() >= MSG_HEADER_SIZE)
            {
                std::vector<uint8_t> header(reply.begin(), reply.begin() + MSG_HEADER_SIZE);
                write_u32(header, 4, static_cast<uint32_t>(reply.size() + trailer.size()));
                (void)emu.memory.try_write_memory(target, header.data(), header.size());
            }

            return msgr::rcv_too_large;
        }

        if (!emu.memory.try_write_memory(target, reply.data(), reply.size()))
        {
            return msgr::rcv_invalid_data;
        }

        if (!emu.memory.try_write_memory(target + reply.size(), trailer.data(), trailer.size()))
        {
            return msgr::rcv_invalid_data;
        }

        return deliver_empty_aux(emu, call);
    }

    mach_msg_return_t perform_msg(macos_emulator& emu, const msg_call& call)
    {
        if (call.decode_error != msgr::success)
        {
            return call.decode_error;
        }

        auto reply_delivered = false;

        if ((call.options & msg_option::send_msg) != 0)
        {
            if (auto* sender = emu.process.active_thread; sender != nullptr && call.header.remote_port != PORT_NULL)
            {
                sender->last_send_port = call.header.remote_port;
                sender->last_send_routine = call.header.id;
            }

            report_dropped_aux(emu, call);

            if (call.send_size < MSG_HEADER_SIZE)
            {
                return msgr::send_msg_too_small;
            }

            if (call.send_size > MSG_SIZE_MAX)
            {
                return msgr::send_invalid_data;
            }

            auto* destination = emu.mach.ports.find(call.header.remote_port);
            if (call.header.remote_port == PORT_NULL || destination == nullptr)
            {
                // MACH_SEND_INVALID_DEST is a value a caller may legitimately ignore, so a name the
                // emulator never minted disappears into a library's error path and surfaces later as
                // an unexplained wait. Naming it once per (port, routine) is what makes it findable.
                static std::set<std::pair<port_name_t, int32_t>> reported{};
                if (reported.emplace(call.header.remote_port, call.header.id).second)
                {
                    emu.log.warn("mach_msg send of routine 0x%x to port 0x%x: no such port in this namespace\n", call.header.id,
                                 call.header.remote_port);
                }

                return msgr::send_invalid_dest;
            }

            const auto body = read_message_body(emu, call);
            if (body.size() != static_cast<size_t>(call.send_size) - MSG_HEADER_SIZE)
            {
                return msgr::send_invalid_data;
            }

            if (destination_is_kernel_object(emu, call.header.remote_port))
            {
                auto reply = dispatch_kernel_message(emu, call, body, emu.mach.ports.object_of(call.header.remote_port).kind);

                if ((call.options & msg_option::rcv_msg) != 0)
                {
                    if (!reply.empty())
                    {
                        const auto result = deliver_reply(emu, call, reply);
                        if (result == msgr::rcv_too_large)
                        {
                            // A message the receive buffer cannot hold stays queued: that is the whole
                            // contract of MACH_RCV_LARGE, and the caller comes back with a bigger buffer
                            // and no send. sogen synthesises the reply during the send, so the only place
                            // it can wait is the reply port's own queue -- dropping it here leaves the
                            // retry parked on an empty port forever.
                            const auto pending = call.rcv_name != PORT_NULL ? call.rcv_name : call.header.local_port;
                            if (auto* reply_port = emu.mach.ports.find(pending); reply_port != nullptr)
                            {
                                reply_port->queue.push_back(std::move(reply));
                            }
                        }

                        if (result != msgr::success)
                        {
                            return result;
                        }

                        reply_delivered = true;
                    }
                }
                else if (!reply.empty())
                {
                    // No reply port is a MIG simpleroutine, or an XPC message sent one-way: the sender is
                    // not going to receive anything, so a reply queued anywhere is one nobody drains, and
                    // an app that posts notifications adds one per post.
                    if (call.header.local_port == PORT_NULL)
                    {
                        static std::set<int32_t> reported{};
                        if (reported.insert(call.header.id).second)
                        {
                            emu.log.info("routine 0x%x is sent one-way; its reply is not queued\n", call.header.id);
                        }
                    }
                    else
                    {
                        // The daemon's answer to an asynchronous send is intercepted into the channel's
                        // message queue, not the reply port's: the client drains the channel with the
                        // receive keyed by the channel port, and the message's header keeps naming the
                        // port it was addressed to -- the waiter's handoff check compares exactly that
                        // (measured 2026-08-27 on the host).
                        destination->queue.push_back(std::move(reply));
                        announce_queued_message(emu, call.header.remote_port, call.header.id);
                    }
                }

                // A send-once right is consumed by the single message sent through it. The guest's MIG
                // stub allocates a fresh one per call, so leaving it live would leak a port per RPC.
                auto* reply_right = emu.mach.ports.find(call.header.local_port);
                if (reply_right != nullptr && reply_right->has_send_once)
                {
                    emu.mach.ports.deallocate(call.header.local_port);
                }
            }
            else
            {
                // Nothing in the emulator services a port the guest opened against a daemon, so a message
                // queued here is one nobody will answer. Remembering which routine it was is the whole
                // difference between "the guest is stuck" and "this is the MIG routine to implement next".
                emu.mach.last_unserviced_send = mach_unserviced_send{
                    .port = call.header.remote_port,
                    .routine = call.header.id,
                };

                {
                    static std::set<std::pair<port_name_t, int32_t>> reported{};
                    if (reported.emplace(call.header.remote_port, call.header.id).second)
                    {
                        emu.log.info("routine 0x%x queued on port 0x%x, which no server in sogen reads\n", call.header.id,
                                     call.header.remote_port);
                    }
                }

                if (destination->queue.size() >= destination->queue_limit)
                {
                    return msgr::send_timed_out;
                }

                std::vector<uint8_t> message(MSG_HEADER_SIZE, 0);
                write_msg_header(message, call.header);
                message.insert(message.end(), body.begin(), body.end());
                destination->queue.push_back(std::move(message));

                announce_queued_message(emu, call.header.remote_port, call.header.id);
            }
        }

        if ((call.options & msg_option::rcv_msg) != 0 && !reply_delivered)
        {
            // The mq receive names the channel to drain in the send-side remote slot: the workloop
            // drains carry the channel port there while rcv_name stays the shared reply port
            // (measured 2026-08-27 on the host, options 0x40700420e).
            auto source_name = call.rcv_name;
            if ((call.options & msg_option::mq_call) != 0 && call.header.remote_port != PORT_NULL)
            {
                source_name = call.header.remote_port;
            }

            auto* source = emu.mach.ports.find(source_name);
            if (source == nullptr)
            {
                return msgr::rcv_invalid_name;
            }

            // A receive named on a port set drains whichever member has a message; CFRunLoop's wait port
            // is a set, so the whole run loop hangs without this hop.
            auto* queue_owner = source;
            if (source->is_port_set)
            {
                queue_owner = emu.mach.ports.first_queued_member(source_name);
            }

            if (queue_owner != nullptr && !queue_owner->queue.empty())
            {
                source = queue_owner;
                source_name = make_port_name(source->index, source->generation);
                auto message = std::move(source->queue.front());
                source->queue.pop_front();

                // ipc_kmsg_copyout_header names the port the message was received on in
                // msgh_local_port, and CFRunLoop reads that field to tell which of its sources woke it:
                // __CFRunLoopRun compares it against the main dispatch queue's port, so a notification
                // that arrives with a null local port is a wake for nothing and the run loop goes
                // straight back to sleep. Only a header that carries nothing there is filled in -- a
                // sender's own local port is the reply right it attached, and sogen delivers messages
                // verbatim, so overwriting it would lose the right the receiver has to answer on.
                if (message.size() >= MSG_HEADER_SIZE)
                {
                    auto header = read_msg_header(message);
                    if (header.local_port == PORT_NULL)
                    {
                        header.local_port = source_name;
                        write_msg_header(message, header);
                    }
                }

                // EVFILT_MACHPORT is level-triggered: a drain that leaves the port non-empty fires the
                // knote again, or a worker that already consumed the coalesced event parks over what is
                // still queued.
                if (!source->queue.empty() && emu.process.kqueues.note_port_message(source_name) != 0)
                {
                    emu.workqueue.wake_parked_worker(emu);
                }

                return deliver_reply(emu, call, message);
            }

            // A thread that reaches its own receive again has been rescheduled onto it, so the park is
            // over whatever happens next.
            if (auto* waiter = emu.process.active_thread; waiter != nullptr)
            {
                waiter->blocked_on_port = 0;
            }

            if ((call.options & msg_option::rcv_timeout) != 0)
            {
                return msgr::rcv_timed_out;
            }

            // A receive that found nothing is where a thread runs out of work, and a repaint the guest
            // has already committed is what that thread is borrowed for. An update that changes only
            // CoreAnimation commits to the render server and never reaches SkyLight, so
            // _SLSTransactionCommit -- the frame cadence for everything else -- stops running once an
            // application has settled, and the repaint is left stranded in the layer tree. Before the
            // reschedule, not after: a run loop with a timer armed against its own wait port always has
            // a deadline for the scheduler to fire, so the park below is never reached.
            if (emu.borrow_a_waiting_thread_for_a_frame())
            {
                return msgr::rcv_interrupted;
            }

            // Another thread may still be able to run and enqueue on this port, so the waiter parks
            // instead of halting: its pc is rewound onto its own svc, so scheduling it again re-runs the
            // receive, the way a kernel restarts an interrupted syscall.
            if (auto* waiter = emu.process.active_thread; waiter != nullptr)
            {
                waiter->blocked_on_port = source_name;

                if (emu.reschedule_away_from_a_blocked_thread())
                {
                    return msgr::rcv_interrupted;
                }

                waiter->blocked_on_port = 0;
            }

            // An application that has finished launching parks exactly here, with every other thread
            // parked behind it, and from inside the guest that is indistinguishable from a hang. What
            // separates them is whether anything outside the guest can still enqueue: with a window or a
            // page attached this is the run loop waiting for a click, not a deadlock.
            if (emu.can_wake_from_host_input())
            {
                emu.park_for_host_input();
                return msgr::rcv_interrupted;
            }

            // Nothing else can run, so nothing can ever enqueue here. Halting names the real condition
            // instead, together with the message that went unanswered -- a reply is only ever awaited
            // because a request was sent, and that request is what is missing.
            // How many threads exist is the difference between "nothing else could run" and "something
            // else could have, and the emulator did not switch to it".
            size_t runnable = 0;
            for (const auto& [id, thread] : emu.process.threads)
            {
                runnable += thread.terminated ? 0u : 1u;
            }

            auto detail = "mach_msg receive on an empty port with no timeout, port=" + std::to_string(call.rcv_name) + ", " +
                          std::to_string(runnable) + " live thread" + (runnable == 1 ? "" : "s");

            if (emu.mach.last_unserviced_send.routine != 0)
            {
                detail += "; no server answered routine " + std::to_string(emu.mach.last_unserviced_send.routine) + " sent to port " +
                          std::to_string(emu.mach.last_unserviced_send.port);
            }

            // Which library decided to wait is the question this stop exists to answer, and the port
            // number alone never answers it.
            for (const auto& frame : emu.backtrace(8))
            {
                detail += "\n    " + frame;
            }

            emu.record_stop(stop_reason::mach_receive_deadlock, detail);
            emu.stop();
            return msgr::rcv_timed_out;
        }

        return msgr::success;
    }
}
