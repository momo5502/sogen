#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_exception.hpp>

#include <algorithm>
#include <ranges>
#include <cstring>
#include <string_view>

namespace
{
    constexpr uint64_t code_base = 0x100000000ULL;
    constexpr uint64_t scratch = 0x340000000ULL;

    TEST(MachException, AnUndefinedInstructionBecomesSigill)
    {
        const auto emu = macos_test::make_emulator();
        macos_test::write_guest_code(*emu, code_base,
                                     {
                                         0xD503201F, // nop
                                         0x00000000, // udf #0
                                     });

        emu->start(8);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        EXPECT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_EQ(emu->mach.last_exception->type, sogen::mach::exception_type::bad_instruction);
        EXPECT_EQ(emu->mach.last_exception->signal, 4);
        EXPECT_FALSE(emu->mach.last_exception->delivered) << "nothing registered an exception port";
        EXPECT_NE(emu->last_stop_detail().find("EXC_BAD_INSTRUCTION"), std::string::npos);
        EXPECT_NE(emu->last_stop_detail().find("SIGILL"), std::string::npos);
        EXPECT_NE(emu->last_stop_detail().find("0x100000004"), std::string::npos);
    }

    TEST(MachException, AnUnmappedReadBecomesSigsegv)
    {
        const auto emu = macos_test::make_emulator();

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 1, 0x900000000ULL);
        words.push_back(0xF9400020); // ldr x0, [x1]

        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size() + 4);

        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_EQ(emu->mach.last_exception->type, sogen::mach::exception_type::bad_access);
        EXPECT_EQ(emu->mach.last_exception->signal, 11);
        EXPECT_NE(emu->last_stop_detail().find("SIGSEGV"), std::string::npos);
    }

    TEST(MachException, SetExceptionPortsIsRecordedAndFound)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        std::vector<uint8_t> body(sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE + sogen::mach::NDR_RECORD_SIZE + 12, 0);
        sogen::mach::write_u32(body, 0, 1);
        sogen::mach::write_port_descriptor(
            std::span{body}.subspan(sogen::mach::MSG_BODY_SIZE),
            {.name = port, .disposition = sogen::mach::disposition::copy_send, .type = sogen::mach::descriptor_type::port});
        std::ranges::copy(sogen::mach::NDR_RECORD, body.begin() + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE);
        constexpr size_t args = sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE + sogen::mach::NDR_RECORD_SIZE;
        sogen::mach::write_u32(body, args, 1u << sogen::mach::exception_type::bad_access); // mask
        sogen::mach::write_u32(body, args + 4, 1);                                         // EXCEPTION_DEFAULT
        sogen::mach::write_u32(body, args + 8, 0);                                         // flavor

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3413, body, 44, true);

        EXPECT_EQ(static_cast<int32_t>(sogen::mach::read_u32(reply, 32)), sogen::mach::kr::success);
        const auto handler = emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access);
        ASSERT_TRUE(handler.has_value());
        EXPECT_EQ(handler->port, port);
        EXPECT_FALSE(emu->mach.exceptions.find_handler(sogen::mach::exception_type::arithmetic).has_value());
    }

    TEST(MachException, AbortWithPayloadSurfacesTheReasonString)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        constexpr std::string_view reason{"assertion failure: \"error\" -> 32"};
        emu->memory.write_memory(scratch, reason.data(), reason.size() + 1);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 0, 5);       // reason namespace
        macos_test::load_x(words, 1, 32);      // reason code
        macos_test::load_x(words, 2, 0);       // payload
        macos_test::load_x(words, 3, 0);       // payload size
        macos_test::load_x(words, 4, scratch); // reason string
        macos_test::load_x(words, 5, 0);       // flags
        words.push_back(0xD2804130);           // mov x16, #521
        words.push_back(0xD4001001);           // svc #0x80

        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size() + 2);

        EXPECT_NE(emu->last_stop_detail().find("assertion failure"), std::string::npos)
            << "this string is the fastest possible diagnosis of a missing mach entry point";
        EXPECT_NE(emu->last_stop_detail().find("32"), std::string::npos);
        ASSERT_TRUE(emu->process.exit_status.has_value());

        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_EQ(emu->mach.last_exception->type, sogen::mach::exception_type::crash);
        EXPECT_EQ(emu->mach.last_exception->code, 5u) << "the reason namespace travels as the exception code";
        EXPECT_EQ(emu->mach.last_exception->subcode, 32u);
        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::signal_termination);
    }

    // An exclusive load unicorn's ARM model rejects for misalignment. It is the only way an abort
    // reaches the interrupt hook rather than the memory-violation hook, so it is the only cover the
    // EXCP_DATA_ABORT mapping can get. Apple silicon does not fault on this instruction at all --
    // measured on the host -- so the test asserts the emulator's classification, not Darwin's.
    TEST(MachException, ADataAbortAtTheInterruptHookIsClassifiedAsBadAccess)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 1, scratch + 1);
        words.push_back(0xC85F7C20); // ldxr x0, [x1]

        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size() + 4);

        EXPECT_EQ(emu->last_stop_reason(), sogen::stop_reason::unhandled_cpu_exception);
        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_EQ(emu->mach.last_exception->type, sogen::mach::exception_type::bad_access);
        EXPECT_EQ(emu->mach.last_exception->signal, 11);
        EXPECT_NE(emu->last_stop_detail().find("index=4"), std::string::npos);
    }

    TEST(MachException, AProtectionFailureCarriesADifferentCodeThanAnUnmappedAccess)
    {
        const auto unmapped = macos_test::make_emulator();
        {
            std::vector<uint32_t> words{};
            macos_test::load_x(words, 1, 0x900000000ULL);
            words.push_back(0xF9400020); // ldr x0, [x1]
            macos_test::write_guest_code(*unmapped, code_base, words);
            unmapped->start(words.size() + 4);
        }

        const auto readonly = macos_test::make_emulator();
        {
            readonly->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read);

            std::vector<uint32_t> words{};
            macos_test::load_x(words, 1, scratch);
            words.push_back(0xF9000020); // str x0, [x1]
            macos_test::write_guest_code(*readonly, code_base, words);
            readonly->start(words.size() + 4);
        }

        ASSERT_TRUE(unmapped->mach.last_exception.has_value());
        ASSERT_TRUE(readonly->mach.last_exception.has_value());
        EXPECT_EQ(unmapped->mach.last_exception->code, sogen::mach::kr::invalid_address);
        EXPECT_EQ(readonly->mach.last_exception->code, sogen::mach::kr::protection_failure);
        EXPECT_EQ(readonly->mach.last_exception->subcode, scratch);
    }

    TEST(MachException, ARegisteredHandlerReceivesTheExceptionMessage)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        ASSERT_EQ(emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::bad_instruction, port,
                                                 sogen::mach::exception_behavior::defaults | sogen::mach::exception_behavior::mach_codes,
                                                 0),
                  sogen::mach::kr::success);

        macos_test::write_guest_code(*emu, code_base, {0x00000000}); // udf #0
        emu->start(4);

        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_TRUE(emu->mach.last_exception->delivered);

        const auto* entry = emu->mach.ports.find(port);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->queue.size(), 1u);

        const auto& message = entry->queue.front();
        const auto header = sogen::mach::read_msg_header(message);
        EXPECT_EQ(header.id, 2405) << "MACH_EXCEPTION_CODES selects mach_exception_raise, not exception_raise";
        EXPECT_EQ(header.remote_port, port);
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u);

        constexpr size_t args = sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE + 2 * sogen::mach::PORT_DESCRIPTOR_SIZE +
                                sogen::mach::NDR_RECORD_SIZE;
        EXPECT_EQ(sogen::mach::read_u32(message, args), sogen::mach::exception_type::bad_instruction);
        EXPECT_EQ(sogen::mach::read_u32(message, args + 4), 2u) << "codeCnt";
    }

    // Without a handler nothing may be queued anywhere; a guest that never registered one must not see
    // an exception message appear on an unrelated port.
    TEST(MachException, AnUnmaskedExceptionIsNotDelivered)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        ASSERT_EQ(emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::arithmetic, port,
                                                 sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::success);

        macos_test::write_guest_code(*emu, code_base, {0x00000000}); // udf #0
        emu->start(4);

        ASSERT_TRUE(emu->mach.last_exception.has_value());
        EXPECT_FALSE(emu->mach.last_exception->delivered);
        EXPECT_TRUE(emu->mach.ports.find(port)->queue.empty());
    }

    std::vector<uint8_t> exception_ports_body(const sogen::mach::port_name_t port, const uint32_t mask, const uint32_t behavior)
    {
        constexpr size_t args = sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE + sogen::mach::NDR_RECORD_SIZE;

        std::vector<uint8_t> body(args + 12, 0);
        sogen::mach::write_u32(body, 0, 1);
        sogen::mach::write_port_descriptor(
            std::span{body}.subspan(sogen::mach::MSG_BODY_SIZE),
            {.name = port, .disposition = sogen::mach::disposition::copy_send, .type = sogen::mach::descriptor_type::port});
        std::ranges::copy(sogen::mach::NDR_RECORD, body.begin() + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE);
        sogen::mach::write_u32(body, args, mask);
        sogen::mach::write_u32(body, args + 4, behavior);
        sogen::mach::write_u32(body, args + 8, 0);
        return body;
    }

    TEST(MachException, AThreadLevelHandlerWinsOverTheTaskLevelOne)
    {
        const auto emu = macos_test::make_emulator();
        const auto task_port = emu->mach.ports.allocate_receive_right();
        const auto thread_port = emu->mach.ports.allocate_receive_right();
        const auto mask = 1u << sogen::mach::exception_type::bad_access;

        const auto task_reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3413, exception_ports_body(task_port, mask, 1), 44, true);
        ASSERT_EQ(static_cast<int32_t>(sogen::mach::read_u32(task_reply, 32)), sogen::mach::kr::success);

        ASSERT_TRUE(emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access).has_value());
        EXPECT_EQ(emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access)->port, task_port);

        const auto thread_reply = macos_test::send_mig_call(*emu, emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID), 3613,
                                                            exception_ports_body(thread_port, mask, 1), 44, true);
        ASSERT_EQ(static_cast<int32_t>(sogen::mach::read_u32(thread_reply, 32)), sogen::mach::kr::success);

        const auto handler = emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access);
        ASSERT_TRUE(handler.has_value());
        EXPECT_EQ(handler->port, thread_port) << "the thread-level handler is consulted first";

        // Both registrations carry the same mask, so preferring the thread entry is indistinguishable
        // from having overwritten the task entry until the thread entry is taken away again.
        macos_test::send_mig_call(*emu, emu->mach.thread_self_for(sogen::MACH_MAIN_THREAD_ID), 3613,
                                  exception_ports_body(sogen::mach::PORT_NULL, mask, 1), 44, true);

        const auto surviving = emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access);
        ASSERT_TRUE(surviving.has_value()) << "clearing the thread handler must not disturb the task handler";
        EXPECT_EQ(surviving->port, task_port);
    }

    TEST(MachException, GetExceptionPortsReportsWhatWasRegistered)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();
        const auto mask = 1u << sogen::mach::exception_type::bad_instruction;

        macos_test::send_mig_call(*emu, emu->mach.task_self, 3413, exception_ports_body(port, mask, 1), 44, true);

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3414, macos_test::ndr_body({mask}), 96);
        const auto header = sogen::mach::read_msg_header(reply);
        ASSERT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u) << "the reply carries the handler port as a descriptor";

        sogen::mach::port_descriptor descriptor{};
        std::memcpy(&descriptor.name, reply.data() + sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE, sizeof(descriptor.name));
        EXPECT_EQ(descriptor.name, port);
    }

    TEST(MachException, TheHardenedExceptionHandlerRoutineIsRefused)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3465, macos_test::ndr_body({0}), 44);
        ASSERT_EQ(sogen::mach::read_msg_header(reply).size, sogen::mach::MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(sogen::mach::read_u32(reply, 32)), sogen::mach::kr::not_supported);
    }

    // task_set_exception_ports claims every type in the mask. A later, narrower registration therefore
    // has to take those types away from the earlier handler while leaving the rest of its mask alone.
    TEST(MachException, ANarrowerRegistrationOnlyTakesTheTypesItClaims)
    {
        const auto emu = macos_test::make_emulator();
        const auto broad_port = emu->mach.ports.allocate_receive_right();
        const auto narrow_port = emu->mach.ports.allocate_receive_right();

        constexpr auto broad = (1u << sogen::mach::exception_type::bad_access) | (1u << sogen::mach::exception_type::arithmetic);
        constexpr auto narrow = 1u << sogen::mach::exception_type::bad_access;

        ASSERT_EQ(emu->mach.exceptions.set_ports(false, broad, broad_port, sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::success);
        ASSERT_EQ(emu->mach.exceptions.set_ports(false, narrow, narrow_port, sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::success);

        const auto claimed = emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access);
        const auto untouched = emu->mach.exceptions.find_handler(sogen::mach::exception_type::arithmetic);

        ASSERT_TRUE(claimed.has_value());
        ASSERT_TRUE(untouched.has_value());
        EXPECT_EQ(claimed->port, narrow_port);
        EXPECT_EQ(untouched->port, broad_port) << "arithmetic was never claimed by the second registration";
    }

    TEST(MachException, SetExceptionPortsRejectsAnEmptyMaskAndAnUnknownBehavior)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        EXPECT_EQ(emu->mach.exceptions.set_ports(false, 0, port, sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::invalid_argument);
        EXPECT_EQ(emu->mach.exceptions.set_ports(false, 1, port, 0, 0), sogen::mach::kr::invalid_argument);
        EXPECT_EQ(emu->mach.exceptions.set_ports(false, 1, port, 9, 0), sogen::mach::kr::invalid_argument);
        EXPECT_FALSE(emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access).has_value());
    }

    TEST(MachException, ADisjointRegistrationLeavesExistingHandlersAlone)
    {
        const auto emu = macos_test::make_emulator();
        const auto arithmetic_port = emu->mach.ports.allocate_receive_right();
        const auto access_port = emu->mach.ports.allocate_receive_right();

        ASSERT_EQ(emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::arithmetic, arithmetic_port,
                                                 sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::success);
        ASSERT_EQ(emu->mach.exceptions.set_ports(false, 1u << sogen::mach::exception_type::bad_access, access_port,
                                                 sogen::mach::exception_behavior::defaults, 0),
                  sogen::mach::kr::success);

        const auto arithmetic = emu->mach.exceptions.find_handler(sogen::mach::exception_type::arithmetic);
        ASSERT_TRUE(arithmetic.has_value()) << "a registration for one type must not drop a handler for another";
        EXPECT_EQ(arithmetic->port, arithmetic_port);
        EXPECT_EQ(emu->mach.exceptions.find_handler(sogen::mach::exception_type::bad_access)->port, access_port);
    }

    TEST(MachException, ReRegisteringTheSameMaskReplacesTheHandler)
    {
        const auto emu = macos_test::make_emulator();
        const auto first = emu->mach.ports.allocate_receive_right();
        const auto second = emu->mach.ports.allocate_receive_right();
        constexpr auto mask = 1u << sogen::mach::exception_type::bad_access;

        emu->mach.exceptions.set_ports(false, mask, first, sogen::mach::exception_behavior::defaults, 0);
        emu->mach.exceptions.set_ports(false, mask, second, sogen::mach::exception_behavior::defaults, 0);

        macos_test::write_guest_code(*emu, code_base, {0x00000000}); // udf #0
        emu->memory.allocate_memory(scratch, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        std::vector<uint32_t> words{};
        macos_test::load_x(words, 1, 0x900000000ULL);
        words.push_back(0xF9400020); // ldr x0, [x1]
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size() + 4);

        EXPECT_TRUE(emu->mach.ports.find(first)->queue.empty()) << "the replaced handler must not still receive";
        EXPECT_EQ(emu->mach.ports.find(second)->queue.size(), 1u);
    }
}
