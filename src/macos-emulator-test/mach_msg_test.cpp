#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_msg.hpp>
#include <mach/mach_types.hpp>
#include <screenshot_ui_backend.hpp>

#include <algorithm>
#include <string>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t msg_base = 0x340000000ULL;

    macos_test::mach_msg2_args mig_call_args(sogen::macos_emulator& emu, const uint32_t id, const uint32_t send_size,
                                             const uint32_t rcv_size)
    {
        return {
            .buffer = msg_base,
            .options = msg_option::send_msg | msg_option::rcv_msg,
            .bits = make_bits(disposition::copy_send, disposition::make_send_once),
            .send_size = send_size,
            .remote_port = emu.mach.task_self,
            .local_port = emu.mach.make_special_reply_port(1),
            .voucher_port = 0,
            .id = id,
            .descriptor_count = 0,
            .rcv_name = emu.mach.make_special_reply_port(1),
            .rcv_size = rcv_size,
            .priority = 0,
            .timeout = 0,
        };
    }

    TEST(MachMsg, UnknownRoutineIdProducesAMigBadIdReply)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto args = mig_call_args(*emu, 9999, 40, 64);
        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), 0u) << "the trap succeeds; the failure is in the reply";

        std::array<uint8_t, MIG_REPLY_ERROR_SIZE> reply{};
        emu->memory.read_memory(msg_base, reply.data(), reply.size());
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(header.id, 9999 + 100);
        // The received shape: the reply arrived on the reply port, and carries no right back.
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(header.local_port, args.local_port);
        EXPECT_EQ(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), mig_error::bad_id);
    }

    TEST(MachMsg, TheTrailerFollowsTheReplyAndIsNotCountedInMsghSize)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto words = macos_test::mach_msg2_words(mig_call_args(*emu, 9999, 40, 64));
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        std::array<uint8_t, MIG_REPLY_ERROR_SIZE + TRAILER_SIZE> reply{};
        emu->memory.read_memory(msg_base, reply.data(), reply.size());

        EXPECT_EQ(read_u32(reply, 4), MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(read_u32(reply, MIG_REPLY_ERROR_SIZE), 0u) << "MACH_MSG_TRAILER_FORMAT_0";
        EXPECT_EQ(read_u32(reply, MIG_REPLY_ERROR_SIZE + 4), TRAILER_SIZE);
    }

    TEST(MachMsg, SendToAnUnknownPortIsRejected)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        auto args = mig_call_args(*emu, 200, 40, 64);
        args.remote_port = 0x9999;
        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::send_invalid_dest);
    }

    // The "2-byte message" this used to expect success for was a misread MACH64_MSG_VECTOR element
    // count. A flat mach_msg2 send really does have the classic header floor, and answers anything below
    // it with MACH_SEND_MSG_TOO_SMALL -- the same value the host returns for a vector whose message
    // element declares no bytes (measured 2026-08-28).
    TEST(MachMsg, AnUndersizedSendIsTooSmall)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto words = macos_test::mach_msg2_words(mig_call_args(*emu, 200, 16, 64));
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::send_msg_too_small);
    }

    TEST(MachMsg, AReceiveBufferTooSmallForTheReplyIsRejected)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto words = macos_test::mach_msg2_words(mig_call_args(*emu, 9999, 40, 8));
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::rcv_too_large);
    }

    TEST(MachMsg, ABlockingReceiveOnAnEmptyPortHaltsInsteadOfHanging)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;
        args.timeout = 0;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size() + 8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock)
            << "a receive that can never be satisfied must halt, not spin";

        // The port number alone never says which library decided to wait, and that is the only thing
        // worth knowing here. Measured value: a real CFPreferences hang reports
        // xpc_connection_send_message_with_reply_sync under __CFPrefsImpersonateApplication..., which is
        // what turned "the guest is stuck on port 3073" into "sogen has no cfprefsd".
        const auto detail = emu->last_stop_detail();
        EXPECT_NE(detail.find("port=" + std::to_string(port)), std::string::npos);
        EXPECT_NE(detail.find("0x"), std::string::npos) << "the stop carries the stack that reached the receive";

        // "1 live thread" is what separates a real deadlock from a scheduler that never switched away.
        EXPECT_NE(detail.find("live thread"), std::string::npos);
    }

    TEST(MachMsg, ATimedReceiveOnAnEmptyPortTimesOutImmediately)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg | msg_option::rcv_timeout;
        args.rcv_name = port;
        args.rcv_size = 256;
        args.timeout = 50;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::rcv_timed_out);
        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock);
    }

    TEST(MachMsg, QueuedMessagesAreReceivedInOrder)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();
        auto* entry = emu->mach.ports.find(port);
        ASSERT_NE(entry, nullptr);

        for (const uint32_t id : {1001u, 1002u})
        {
            std::vector<uint8_t> message(MSG_HEADER_SIZE, 0);
            write_msg_header(message, {.bits = 0x1513,
                                       .size = MSG_HEADER_SIZE,
                                       .remote_port = PORT_NULL,
                                       .local_port = port,
                                       .voucher_port = 0,
                                       .id = static_cast<int32_t>(id)});
            entry->queue.push_back(std::move(message));
        }

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);

        std::array<uint8_t, MSG_HEADER_SIZE> received{};
        emu->memory.read_memory(msg_base, received.data(), received.size());
        EXPECT_EQ(read_msg_header(received).id, 1001);
        EXPECT_EQ(emu->mach.ports.find(port)->queue.size(), 1u);
    }

    // A wait is only a deadlock when nothing else can run. With another thread available the waiter parks
    // on its own svc and the emulator switches away, which is what a kernel does with an interrupted
    // syscall -- and what has to work before a workqueue thread can ever service the queue a blocked
    // thread is waiting on.
    TEST(MachMsg, ABlockingReceiveParksTheWaiterAndSwitchesAway)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr uint64_t other_code = code_base + 0x800;
        constexpr uint64_t waiter_stack = 0x321000000ULL;
        constexpr uint64_t other_stack = 0x322000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(waiter_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(other_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;
        args.timeout = 0;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        // Ends in an exit rather than a ret: a new thread's lr is zero, so returning would fault at zero
        // and the run would stop for a reason that has nothing to do with what is under test.
        macos_test::write_guest_code(*emu, other_code,
                                     {
                                         0xD2801BE9, // mov x9, #0xDF
                                         0xD2800520, // mov x0, #41
                                         0xD2800030, // mov x16, #1
                                         0xD4001001, // svc #0x80   (exit)
                                     });

        // A bare emulator has no threads, so both are made here rather than assuming a main one exists.
        const auto waiter = emu->process.create_thread(waiter_stack, sogen::MACOS_PAGE_SIZE, code_base);
        const auto second = emu->process.create_thread(other_stack, sogen::MACOS_PAGE_SIZE, other_code);
        ASSERT_TRUE(emu->activate_thread(waiter));

        emu->start();

        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock)
            << "another thread could run, so the wait was not a deadlock";
        EXPECT_EQ(emu->process.exit_status, 41) << "the other thread ran to its own exit";
        EXPECT_EQ(emu->process.active_thread->thread_id, second) << "the emulator switched to it";
        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x9), 0xDFu) << "and it made progress";

        const auto& parked = emu->process.threads.at(waiter);
        EXPECT_EQ(parked.blocked_on_port, port) << "the waiter is parked on the port it asked for";
        EXPECT_EQ(parked.saved_regs.pc, code_base + (words.size() - 1) * 4)
            << "its pc sits on its own svc, so being scheduled again re-runs the receive";
    }

    // The other half: with nothing else runnable it is a real deadlock and still halts.
    TEST(MachMsg, AWaitWithNoOtherRunnableThreadIsStillADeadlock)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;
        args.timeout = 0;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        constexpr uint64_t only_stack = 0x323000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(only_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const auto only = emu->process.create_thread(only_stack, sogen::MACOS_PAGE_SIZE, code_base);
        ASSERT_TRUE(emu->activate_thread(only));

        emu->start(words.size() + 8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock);
        EXPECT_EQ(emu->process.threads.at(only).blocked_on_port, 0u) << "a thread that could not be parked is not left marked";
        EXPECT_EQ(emu->emu().read_instruction_pointer(), code_base + words.size() * 4)
            << "and the rewind was undone, so the halt reports the pc after the trap";
    }

    // The same wait, with something outside the guest that can still deliver. A GUI application that has
    // finished launching parks exactly here with every other thread parked behind it, and halting there
    // is what stops it ever being clicked.
    TEST(MachMsg, AWaitParksForTheHostWhenAnInputSourceIsAttached)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        auto backend = sogen::create_screenshot_ui_backend();
        static_cast<sogen::screenshot_ui_backend*>(backend.get())->set_input_source(true);
        emu->set_ui_backend(std::move(backend));

        size_t polls = 0;
        emu->on_host_idle = [&] {
            if (++polls >= 3)
            {
                emu->stop();
            }
        };

        const auto port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;
        args.timeout = 0;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);

        constexpr uint64_t only_stack = 0x326000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(only_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const auto only = emu->process.create_thread(only_stack, sogen::MACOS_PAGE_SIZE, code_base);
        ASSERT_TRUE(emu->activate_thread(only));

        emu->start(words.size() + 8);

        EXPECT_NE(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock)
            << "a host that can still deliver is the difference between waiting and hanging";
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::explicit_stop) << "only the stop ended this run";
        EXPECT_GE(polls, 3u) << "the emulator polled the host instead of halting on the first empty receive";
        EXPECT_EQ(emu->emu().read_instruction_pointer(), code_base + (words.size() - 1) * 4)
            << "the waiter is left on its own svc, so the receive runs again once something arrives";
    }

    // Two threads both waiting on ports nothing will ever fill. Without skipping threads that are already
    // parked, each would hand control to the other forever and the emulator would spin instead of
    // reporting the deadlock. The instruction budget is what makes that failure visible as a failure
    // rather than as a hung test.
    TEST(MachMsg, TwoThreadsBothWaitingIsStillADeadlock)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr uint64_t second_code = code_base + 0x800;
        constexpr uint64_t first_stack = 0x324000000ULL;
        constexpr uint64_t second_stack = 0x325000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(first_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(second_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto first_port = emu->mach.ports.allocate_receive_right();
        const auto second_port = emu->mach.ports.allocate_receive_right();

        const auto words_for = [&](const sogen::mach::port_name_t port) {
            macos_test::mach_msg2_args args{};
            args.buffer = msg_base;
            args.options = msg_option::rcv_msg;
            args.rcv_name = port;
            args.rcv_size = 256;
            args.timeout = 0;
            return macos_test::mach_msg2_words(args);
        };

        const auto first_words = words_for(first_port);
        const auto second_words = words_for(second_port);
        macos_test::write_guest_code(*emu, second_code, second_words);
        macos_test::write_guest_code(*emu, code_base, first_words);

        const auto first = emu->process.create_thread(first_stack, sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.create_thread(second_stack, sogen::MACOS_PAGE_SIZE, second_code);
        ASSERT_TRUE(emu->activate_thread(first));

        emu->start((first_words.size() + second_words.size()) * 4 + 64);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::mach_receive_deadlock)
            << "neither thread can be woken, so this is a deadlock and not a scheduling decision";
    }

    // Task 6 review finding: only the mq reply enqueue fired note_port_message, so a classic send onto a
    // knote-monitored port queued the message without ever making an EVFILT_MACHPORT event deliverable.
    TEST(MachMsg, AClassicSendFiresAMachportKnoteRegisteredOnTheDestination)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();
        const auto kq = emu->process.kqueues.create();

        sogen::kevent_registration registration{};
        registration.ident = port;
        registration.filter = sogen::MACOS_EVFILT_MACHPORT;
        registration.flags = 0x5;
        registration.udata = 0xDEAD;
        emu->process.kqueues.find(kq)->registrations.push_back(registration);

        macos_test::mach_msg2_args send{};
        send.buffer = msg_base;
        send.options = msg_option::send_msg;
        send.send_size = MSG_HEADER_SIZE;
        send.remote_port = port;
        send.id = 0xC0DE;

        const auto words = macos_test::mach_msg2_words(send);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);
        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 1u) << "the send fires the port's knote, not just queues the message";

        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(events[0].ident, static_cast<uint64_t>(port));
        EXPECT_EQ(events[0].data, static_cast<int64_t>(port));
        EXPECT_EQ(events[0].udata, 0xDEADu);
    }

    // EVFILT_MACHPORT is level-triggered: the event coalesces into one per knote, so a drain that
    // leaves the port non-empty has to fire it again -- otherwise a worker that already consumed the
    // event with its first drain parks over the messages still queued.
    TEST(MachMsg, DrainingOneOfTwoQueuedMessagesReFiresTheMachportKnote)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();
        auto* entry = emu->mach.ports.find(port);
        ASSERT_NE(entry, nullptr);

        const auto kq = emu->process.kqueues.create();
        sogen::kevent_registration registration{};
        registration.ident = port;
        registration.filter = sogen::MACOS_EVFILT_MACHPORT;
        registration.flags = 0x5;
        emu->process.kqueues.find(kq)->registrations.push_back(registration);

        for (const uint32_t id : {1001u, 1002u})
        {
            std::vector<uint8_t> message(MSG_HEADER_SIZE, 0);
            write_msg_header(message, {.bits = 0x1513,
                                       .size = MSG_HEADER_SIZE,
                                       .remote_port = PORT_NULL,
                                       .local_port = port,
                                       .voucher_port = 0,
                                       .id = static_cast<int32_t>(id)});
            entry->queue.push_back(std::move(message));
        }

        ASSERT_EQ(emu->process.kqueues.note_port_message(port), 1u);

        // The worker consumes the coalesced event before draining the first message.
        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        ASSERT_EQ(emu->process.kqueues.pending_count(kq), 0u);

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::rcv_msg;
        args.rcv_name = port;
        args.rcv_size = 256;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);
        EXPECT_EQ(emu->mach.ports.find(port)->queue.size(), 1u) << "one of the two messages is still queued";

        EXPECT_EQ(emu->process.kqueues.pending_count(kq), 1u) << "the knote re-fires for what is still queued";
        ASSERT_EQ(emu->process.kqueues.deliver(kq, events, 1), 1u);
        EXPECT_EQ(events[0].filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(events[0].ident, static_cast<uint64_t>(port));
    }

    void write_vector(sogen::macos_emulator& emu, const uint64_t address, const std::vector<msg_vector_element>& elements)
    {
        for (size_t index = 0; index < elements.size(); ++index)
        {
            std::array<uint8_t, MSG_VECTOR_ELEMENT_SIZE> bytes{};
            write_msg_vector_element(bytes, elements[index]);
            emu.memory.write_memory(address + index * MSG_VECTOR_ELEMENT_SIZE, bytes.data(), bytes.size());
        }
    }

    // Measured 2026-08-28 on the host, breaking on mach_msg2_internal in an AppKit process: a
    // MACH64_MSG_VECTOR call passes an array of mach_msg_vector_t and puts the element *count* in both
    // size halves of the trap. libnotify's checkins ride this form with an inner msgh_size of 0, which
    // the kernel ignores in favour of msgv_send_size. The id here is outside the answered notify family,
    // so the reply is the bad-id refusal and the transport is what is pinned.
    TEST(MachMsg, AVectorSendTakesItsSizeFromTheMessageElementAndRepliesOverIt)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto service = emu->mach.ports.allocate_receive_right({.kind = kernel_object_kind::xpc_service});
        const auto reply_port = emu->mach.make_special_reply_port(1);

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x800;
        constexpr uint64_t aux = msg_base + 0xC00;

        std::vector<uint8_t> request(MSG_HEADER_SIZE, 0);
        write_msg_header(request,
                         {.bits = 0x131513, .size = 0, .remote_port = service, .local_port = reply_port, .voucher_port = 0, .id = 0x777});
        emu->memory.write_memory(message, request.data(), request.size());

        write_vector(*emu, vector,
                     {
                         {.data = message, .rcv_address = 0, .send_size = MSG_HEADER_SIZE, .rcv_size = 64},
                         {.data = aux, .rcv_address = 0, .send_size = 40, .rcv_size = 128},
                     });

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = msg_option::send_msg | msg_option::rcv_msg | msg_option::mq_call | msg_option::vector;
        args.bits = 0x131513;
        args.send_size = 2;
        args.remote_port = service;
        args.local_port = reply_port;
        args.id = 0x777;
        args.rcv_name = reply_port;
        args.rcv_size = 2;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);

        std::array<uint8_t, MIG_REPLY_ERROR_SIZE + TRAILER_SIZE> reply{};
        emu->memory.read_memory(message, reply.data(), reply.size());
        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.size, MIG_REPLY_ERROR_SIZE) << "the refusal reply overwrites the message element";
        EXPECT_EQ(header.id, 0x777 + 100);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), mig_error::bad_id);
        EXPECT_EQ(read_u32(reply, MIG_REPLY_ERROR_SIZE + 4), TRAILER_SIZE) << "the null trailer follows the reply";

        EXPECT_TRUE(emu->mach.ports.destination_of(service)->queue.empty())
            << "a synchronous reply is delivered in place, never queued on the channel";
    }

    // Same measurement: the send-only members of the family (0x402 register_common_port, 0x3f8
    // cancel_2 -- simpleroutines in notify_ipc.defs) get no reply at all.
    TEST(MachMsg, AVectorSendOnlyCarriesTheElementSizedMessage)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x800;
        constexpr uint32_t real_size = MSG_HEADER_SIZE + 12;

        std::vector<uint8_t> request(real_size, 0xAB);
        write_msg_header(request, {.bits = 0x130013, .size = 0, .remote_port = port, .local_port = 0, .voucher_port = 0, .id = 0x3f8});
        emu->memory.write_memory(message, request.data(), request.size());

        write_vector(*emu, vector, {{.data = message, .rcv_address = 0, .send_size = real_size, .rcv_size = 0}});

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = msg_option::send_msg | msg_option::mq_call | msg_option::vector;
        args.bits = 0x130013;
        args.send_size = 1;
        args.remote_port = port;
        args.id = 0x3f8;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success) << "one element is the message alone, with no aux";

        const auto* destination = emu->mach.ports.destination_of(port);
        ASSERT_EQ(destination->queue.size(), 1u) << "the element-sized message is delivered, not dropped";
        EXPECT_EQ(destination->queue.front().size(), real_size);
        EXPECT_EQ(read_msg_header(destination->queue.front()).size, real_size) << "the kernel writes the size the vector declared";
        EXPECT_EQ(read_msg_header(destination->queue.front()).id, 0x3f8);
    }

    // The shape CFRunLoop's main wait takes, measured 2026-08-28 on the host in __CFRunLoopServiceMachPort:
    // options 0x507000806, both trap size halves carrying the element count 2, and the real 3072-byte
    // receive budget in the message element. Reading that count as a byte budget refused every message
    // the run loop was waiting for with MACH_RCV_TOO_LARGE, which is what left an AppKit app parked in
    // its run loop forever.
    TEST(MachMsg, AVectorReceiveTakesItsByteBudgetFromTheMessageElement)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x400;
        constexpr uint64_t aux = msg_base + 0xE00;
        constexpr size_t av_trailer_size = 68;

        std::vector<uint8_t> queued(MSG_HEADER_SIZE, 0);
        write_msg_header(queued, {.bits = 0x1100, .size = MSG_HEADER_SIZE, .remote_port = PORT_NULL, .local_port = port, .id = 0});
        emu->mach.ports.destination_of(port)->queue.push_back(queued);

        const std::vector<uint8_t> poison(MSG_AUX_HEADER_SIZE, 0xAA);
        emu->memory.write_memory(aux, poison.data(), poison.size());

        write_vector(*emu, vector,
                     {
                         {.data = message, .rcv_address = 0, .send_size = 0, .rcv_size = 0xC00},
                         {.data = aux, .rcv_address = 0, .send_size = 0, .rcv_size = 0x80},
                     });

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = 0x507000806;
        args.send_size = 2;
        args.local_port = port;
        args.rcv_name = port;
        args.rcv_size = 2;
        args.timeout = 0xFFFFFFFF;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);

        std::array<uint8_t, MSG_HEADER_SIZE + av_trailer_size> received{};
        emu->memory.read_memory(message, received.data(), received.size());
        EXPECT_EQ(read_msg_header(received).local_port, port);
        EXPECT_EQ(read_u32(received, MSG_HEADER_SIZE + 4), av_trailer_size) << "MACH_RCV_TRAILER_ELEMENTS(7) asks for the mac trailer";

        std::array<uint8_t, MSG_AUX_HEADER_SIZE> aux_header{};
        emu->memory.read_memory(aux, aux_header.data(), aux_header.size());
        EXPECT_EQ(read_u32(aux_header, 0), 0u) << "a message with no auxiliary data zeroes the aux header";
        EXPECT_EQ(read_u32(aux_header, 4), 0u);
    }

    // msgv_rcv_addr: "if non-zero, use it as rcv address instead". Measured 2026-08-28 by pointing a live
    // CFRunLoop receive's msgv_rcv_addr at a scratch page -- the message landed there and msgv_data was
    // left untouched.
    TEST(MachMsg, AVectorReceiveDeliversIntoTheElementsReceiveAddress)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x400;
        constexpr uint64_t elsewhere = msg_base + 0x800;

        std::vector<uint8_t> queued(MSG_HEADER_SIZE, 0);
        write_msg_header(queued, {.bits = 0x1100, .size = MSG_HEADER_SIZE, .remote_port = PORT_NULL, .local_port = port, .id = 0x2b});
        emu->mach.ports.destination_of(port)->queue.push_back(queued);

        write_vector(*emu, vector, {{.data = message, .rcv_address = elsewhere, .send_size = 0, .rcv_size = 0xC00}});

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = msg_option::rcv_msg | msg_option::vector;
        args.send_size = 1;
        args.rcv_name = port;
        args.rcv_size = 1;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::success);

        std::array<uint8_t, MSG_HEADER_SIZE> received{};
        emu->memory.read_memory(elsewhere, received.data(), received.size());
        EXPECT_EQ(read_msg_header(received).id, 0x2b);

        std::array<uint8_t, MSG_HEADER_SIZE> untouched{};
        emu->memory.read_memory(message, untouched.data(), untouched.size());
        EXPECT_EQ(read_msg_header(untouched).id, 0) << "msgv_data is not written when msgv_rcv_addr names another buffer";
    }

    // The counts xnu refuses, measured 2026-08-28 by forcing them in the register file of a live AppKit
    // process: a third element is MACH_SEND_INVALID_DATA on the send side and MACH_RCV_INVALID_ARGUMENTS
    // on the receive side, and a zero count is MACH_SEND_MSG_TOO_SMALL / MACH_RCV_INVALID_ARGUMENTS.
    struct vector_count_case
    {
        const char* name;
        uint64_t options;
        uint32_t send_count;
        uint32_t rcv_count;
        mach_msg_return_t expected;
    };

    class MachMsgVectorCount : public testing::TestWithParam<vector_count_case>
    {
    };

    TEST_P(MachMsgVectorCount, AnElementCountOutsideTheKernelsRangeIsRefused)
    {
        const auto& parameter = GetParam();

        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x800;

        std::vector<uint8_t> request(MSG_HEADER_SIZE, 0);
        write_msg_header(request, {.bits = 0x130013, .size = 0, .remote_port = port, .local_port = 0, .voucher_port = 0, .id = 0x3f8});
        emu->memory.write_memory(message, request.data(), request.size());

        write_vector(*emu, vector,
                     {
                         {.data = message, .rcv_address = 0, .send_size = MSG_HEADER_SIZE, .rcv_size = 64},
                         {.data = message + 0x100, .rcv_address = 0, .send_size = 0, .rcv_size = 0},
                     });

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = parameter.options | msg_option::vector;
        args.bits = 0x130013;
        args.send_size = parameter.send_count;
        args.remote_port = port;
        args.id = 0x3f8;
        args.rcv_name = port;
        args.rcv_size = parameter.rcv_count;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), parameter.expected);
        EXPECT_TRUE(emu->mach.ports.destination_of(port)->queue.empty()) << "a refused call sends nothing";
    }

    INSTANTIATE_TEST_SUITE_P(MachMsg, MachMsgVectorCount,
                             testing::Values(vector_count_case{"send three", msg_option::send_msg, 3, 0, msgr::send_invalid_data},
                                             vector_count_case{"send none", msg_option::send_msg, 0, 0, msgr::send_msg_too_small},
                                             vector_count_case{"receive three", msg_option::rcv_msg, 2, 3, msgr::rcv_invalid_arguments},
                                             vector_count_case{"receive none", msg_option::rcv_msg, 2, 0, msgr::rcv_invalid_arguments}),
                             [](const testing::TestParamInfo<vector_count_case>& info) {
                                 std::string name{info.param.name};
                                 std::ranges::replace(name, ' ', '_');
                                 return name;
                             });

    // msgv_send_size is the whole size of the message: zero there is MACH_SEND_MSG_TOO_SMALL even though
    // the buffer holds a well-formed header (measured 2026-08-28 on the host).
    TEST(MachMsg, AVectorSendWhoseMessageElementDeclaresNoBytesIsRefused)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto port = emu->mach.ports.allocate_receive_right();

        constexpr uint64_t vector = msg_base;
        constexpr uint64_t message = msg_base + 0x800;

        std::vector<uint8_t> request(MSG_HEADER_SIZE, 0);
        write_msg_header(request,
                         {.bits = 0x130013, .size = MSG_HEADER_SIZE, .remote_port = port, .local_port = 0, .voucher_port = 0, .id = 0x3f8});
        emu->memory.write_memory(message, request.data(), request.size());

        write_vector(*emu, vector, {{.data = message, .rcv_address = 0, .send_size = 0, .rcv_size = 0}});

        macos_test::mach_msg2_args args{};
        args.buffer = vector;
        args.options = msg_option::send_msg | msg_option::vector;
        args.bits = 0x130013;
        args.send_size = 1;
        args.remote_port = port;
        args.id = 0x3f8;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::send_msg_too_small);
        EXPECT_TRUE(emu->mach.ports.destination_of(port)->queue.empty());
    }

    // A vector whose elements cannot be read is a bad address, not a message: reading zeroes out of
    // unmapped memory would send to port zero instead.
    TEST(MachMsg, AVectorAtAnUnmappedAddressIsRefused)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base + 0x40000000ULL;
        args.options = msg_option::send_msg | msg_option::vector;
        args.send_size = 2;

        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        EXPECT_EQ(emu->emu().reg(sogen::arm64_register::x0), msgr::send_invalid_data);
    }

    // The Stage A shape: a worker answers a request and goes back to waiting, and the thread that was
    // parked on the reply has to run again. Only the send clearing the receiver's park makes the
    // reschedule after it pick the receiver -- without it the scheduler skips the still-parked thread
    // and reports a deadlock while its reply sits in the queue.
    TEST(MachMsg, ASendWakesTheThreadParkedOnTheDestinationPort)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr uint64_t sender_code = code_base + 0x800;
        constexpr uint64_t waiter_stack = 0x326000000ULL;
        constexpr uint64_t sender_stack = 0x327000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(waiter_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        ASSERT_TRUE(emu->memory.allocate_memory(sender_stack, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));

        const auto port = emu->mach.ports.allocate_receive_right();
        const auto idle_port = emu->mach.ports.allocate_receive_right();

        macos_test::mach_msg2_args receive{};
        receive.buffer = msg_base;
        receive.options = msg_option::rcv_msg;
        receive.rcv_name = port;
        receive.rcv_size = 256;

        auto waiter_words = macos_test::mach_msg2_words(receive);
        // A successful receive falls through to here, so the exit code is what separates "the waiter ran
        // again" from any other way the run could have stopped.
        waiter_words.push_back(macos_test::movz_x(0, 42, 0));
        waiter_words.push_back(macos_test::movz_x(16, 1, 0));
        waiter_words.push_back(0xD4001001); // svc #0x80   (exit)
        macos_test::write_guest_code(*emu, code_base, waiter_words);

        macos_test::mach_msg2_args send{};
        send.buffer = msg_base;
        send.options = msg_option::send_msg;
        send.send_size = MSG_HEADER_SIZE;
        send.remote_port = port;
        send.id = 0xC0DE;

        macos_test::mach_msg2_args idle{};
        idle.buffer = msg_base;
        idle.options = msg_option::rcv_msg;
        idle.rcv_name = idle_port;
        idle.rcv_size = 256;

        // After answering, the worker parks on a port nothing will fill, so the only way the run ends
        // in the waiter's exit is the reschedule after this park finding the waiter runnable again.
        auto sender_words = macos_test::mach_msg2_words(send);
        const auto idle_words = macos_test::mach_msg2_words(idle);
        sender_words.insert(sender_words.end(), idle_words.begin(), idle_words.end());
        macos_test::write_guest_code(*emu, sender_code, sender_words);

        const auto waiter = emu->process.create_thread(waiter_stack, sogen::MACOS_PAGE_SIZE, code_base);
        emu->process.create_thread(sender_stack, sogen::MACOS_PAGE_SIZE, sender_code);
        ASSERT_TRUE(emu->activate_thread(waiter));

        emu->start(256);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit)
            << "the send woke the waiter, so the sender parking again left a runnable thread";
        EXPECT_EQ(emu->process.exit_status, 42);
        EXPECT_EQ(emu->process.threads.at(waiter).blocked_on_port, 0u);

        std::array<uint8_t, MSG_HEADER_SIZE> received{};
        emu->memory.read_memory(msg_base, received.data(), received.size());
        EXPECT_EQ(read_msg_header(received).id, 0xC0DE) << "the woken waiter's re-run receive drained the queued message";
    }
}
