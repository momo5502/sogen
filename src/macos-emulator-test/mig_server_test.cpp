#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mig_kernel_servers.hpp>

#include <algorithm>
#include <cstring>
#include <mach/mach_types.hpp>
#include <ranges>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    // Keying on msgh_id alone is a latent bug: notifyd's 1012/1023 sit inside the range clock.defs
    // claims, and host_priv 400 collides with bootstrap 400. Registering the same id on two port kinds
    // proves the key is the pair.
    TEST(MigDispatch, TheSameIdOnDifferentPortKindsSelectsDifferentRoutines)
    {
        mig_server_table table{};

        const auto host_routine = [](sogen::macos_emulator&, const mig_request&) { return std::vector<uint8_t>{1}; };
        const auto task_routine = [](sogen::macos_emulator&, const mig_request&) { return std::vector<uint8_t>{2}; };

        table.register_routine(kernel_object_kind::host, 400, host_routine, "host_400");
        table.register_routine(kernel_object_kind::bootstrap, 400, task_routine, "bootstrap_400");

        ASSERT_NE(table.find(kernel_object_kind::host, 400), nullptr);
        ASSERT_NE(table.find(kernel_object_kind::bootstrap, 400), nullptr);
        EXPECT_EQ(table.name_of(kernel_object_kind::host, 400), "host_400");
        EXPECT_EQ(table.name_of(kernel_object_kind::bootstrap, 400), "bootstrap_400")
            << "host_priv 400 and bootstrap 400 must not share a slot";

        EXPECT_EQ(table.find(kernel_object_kind::task, 400), nullptr)
            << "routine ids are only meaningful together with the destination port kind";
        EXPECT_EQ(table.find(kernel_object_kind::host, 3405), nullptr);
    }

    TEST(MigDispatch, AnUnregisteredRoutineFallsBackToMigBadId)
    {
        const auto emu = macos_test::make_emulator();

        sogen::mach::msg_call call{};
        call.header = {.bits = make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = 40,
                       .remote_port = emu->mach.task_self,
                       .local_port = 0x70b,
                       .voucher_port = 0,
                       .id = 99999};

        const std::vector<uint8_t> body{};
        const auto reply = dispatch_kernel_message(*emu, call, body, kernel_object_kind::task);

        ASSERT_EQ(reply.size(), MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(read_msg_header(reply).id, 99999 + 100);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), mig_error::bad_id);
    }

    TEST(MigReplyBuilder, ProducesTheHeaderShapeMigExpects)
    {
        msg_call call{};
        call.header = {.bits = make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = 40,
                       .remote_port = 0x203,
                       .local_port = 0x70b,
                       .voucher_port = 0,
                       .id = 3405};

        mach_port_namespace ports{};
        mig_reply_builder builder{call, ports};
        builder.append_ndr();
        builder.append_u32(0);
        builder.append_u32(8);
        const auto reply = builder.finish();

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, 3505);
        EXPECT_EQ(header.size, reply.size());
        EXPECT_EQ(header.size, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 8);
        // The shape of a *received* message: it arrived on the reply port, and a mig reply conveys no
        // right back, so the remote slot is empty. mig's client checks exactly this and returns
        // MIG_TYPE_ERROR when the remote port is not null.
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(header.local_port, 0x70bu);
        EXPECT_EQ(header.bits & BITS_COMPLEX, 0u);

        const std::array<uint8_t, NDR_RECORD_SIZE> ndr{0, 0, 0, 0, 1, 0, 0, 0};
        EXPECT_TRUE(std::equal(ndr.begin(), ndr.end(), reply.begin() + MSG_HEADER_SIZE));
    }

    // A make_send descriptor is the kernel manufacturing a right for the receiver, so the guest must
    // come out of the exchange holding one. libxpc is what catches the omission: it retains the
    // bootstrap port it was just handed, and mach_port_mod_refs on a name with no send uref fails, which
    // it reports as "Bug in libxpc: Could not create pipe to bootstrap server!" and traps.
    TEST(MigReplyBuilder, AMakeSendDescriptorLeavesTheGuestHoldingASendRight)
    {
        mach_port_namespace ports{};
        const auto name = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 1});
        ASSERT_NE(ports.find(name), nullptr);
        ASSERT_EQ(ports.find(name)->send_urefs, 0u);

        msg_call call{};
        call.header = {.bits = 0x1513, .size = 36, .remote_port = 0x203, .local_port = 0x70b, .voucher_port = 0, .id = 206};

        mig_reply_builder builder{call, ports};
        builder.append_port_descriptor({.name = name, .disposition = disposition::make_send, .type = descriptor_type::port});
        (void)builder.finish();

        EXPECT_EQ(ports.find(name)->send_urefs, 1u);
        EXPECT_EQ(ports.mod_refs(name, right_kind::send, 1), kr::success);
    }

    // The routines that manufacture a right themselves -- mach_port_extract_right is the one that does --
    // hand it over with a move, and a move must not mint a second reference.
    TEST(MigReplyBuilder, AMoveSendDescriptorDoesNotMintASecondReference)
    {
        mach_port_namespace ports{};
        const auto name = ports.allocate_receive_right({.kind = kernel_object_kind::bootstrap, .id = 1});
        ASSERT_EQ(ports.insert_send_right(name), name);
        ASSERT_EQ(ports.find(name)->send_urefs, 1u);

        msg_call call{};
        call.header = {.bits = 0x1513, .size = 36, .remote_port = 0x203, .local_port = 0x70b, .voucher_port = 0, .id = 206};

        mig_reply_builder builder{call, ports};
        builder.append_port_descriptor({.name = name, .disposition = disposition::move_send, .type = descriptor_type::port});
        (void)builder.finish();

        EXPECT_EQ(ports.find(name)->send_urefs, 1u);
    }

    TEST(MigReplyBuilder, AComplexReplyCarriesTheBodyCountAndTheBit)
    {
        msg_call call{};
        call.header = {.bits = 0x1513, .size = 36, .remote_port = 0x203, .local_port = 0x70b, .voucher_port = 0, .id = 206};

        mach_port_namespace ports{};
        mig_reply_builder builder{call, ports};
        builder.set_complex();
        builder.append_port_descriptor({.name = 0x1103, .disposition = disposition::make_send, .type = descriptor_type::port});
        const auto reply = builder.finish();

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, 306);
        EXPECT_EQ(header.size, MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);
        EXPECT_EQ(header.size, 40u) << "the measured reply size for host_get_clock_service";
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 1u) << "descriptor count in mach_msg_body_t";
        EXPECT_EQ(read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE)).name, 0x1103u);
    }

    TEST(MigRequest, ANoArgumentRequestHasNoNdrRecord)
    {
        msg_call call{};
        call.header.id = 8001;
        const std::vector<uint8_t> empty_body{};
        const mig_request request{.call = call, .body = empty_body, .destination = kernel_object_kind::task, .id = 8001};

        EXPECT_FALSE(request.has_ndr()) << "MIG omits the NDR record when a request marshals no arguments";
    }

    TEST(MigRequest, ArgumentsAreReadAfterTheNdrRecord)
    {
        msg_call call{};
        call.header.id = 200;
        std::vector<uint8_t> body(NDR_RECORD_SIZE + 8, 0);
        std::ranges::copy(NDR_RECORD, body.begin());
        write_u32(body, NDR_RECORD_SIZE, 1);      // flavor = HOST_BASIC_INFO
        write_u32(body, NDR_RECORD_SIZE + 4, 12); // out count
        const mig_request request{.call = call, .body = body, .destination = kernel_object_kind::host, .id = 200};

        EXPECT_TRUE(request.has_ndr());
        EXPECT_EQ(request.arg_u32(0), 1u);
        EXPECT_EQ(request.arg_u32(1), 12u);
    }

    TEST(MigRequest, AComplexRequestSkipsTheDescriptorPrefixBeforeTheNdrRecord)
    {
        msg_call call{};
        call.header = {.bits = BITS_COMPLEX | make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = 100,
                       .remote_port = 0x203,
                       .local_port = 0x70b,
                       .voucher_port = 0,
                       .id = 4811};
        call.descriptor_count = 1;

        std::vector<uint8_t> body(76, 0);
        write_u32(body, 0, 1); // descriptor count
        write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE),
                              {.name = PORT_NULL, .disposition = disposition::copy_send, .type = descriptor_type::port});
        std::ranges::copy(NDR_RECORD, body.begin() + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);
        write_u64(body, MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE, 0x300000000ULL);

        const auto request = make_mig_request(call, body, kernel_object_kind::task);

        EXPECT_EQ(request.args_offset, MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE);
        EXPECT_EQ(request.args_offset, 24u) << "mach_vm_map's arguments start 24 bytes into the body";
        EXPECT_EQ(request.arg_u64(0), 0x300000000ULL);
        ASSERT_TRUE(request.descriptor(0).has_value());
        EXPECT_EQ(request.descriptor(0)->disposition, disposition::copy_send);
    }

    TEST(MigHost, HostInfoReturnsAWellFormedHostBasicInfo)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.host_self, 200, macos_test::ndr_body({flavor::host_basic_info, 12}), 320);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 300);
        EXPECT_EQ(header.size, 88u) << "24 header + 8 NDR + 4 RetCode + 4 count + 48 host_basic_info";
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_EQ(read_u32(reply, 36), 12u);

        constexpr size_t info = 40;
        EXPECT_EQ(read_u32(reply, info + 0), emu->system_info.ncpus);
        EXPECT_EQ(read_u32(reply, info + 4), emu->system_info.active_cpus);
        EXPECT_EQ(read_u32(reply, info + 12), CPU_TYPE_ARM64);
        EXPECT_EQ(read_u32(reply, info + 16), CPU_SUBTYPE_ARM64E);
        EXPECT_EQ(read_u32(reply, info + 24), emu->system_info.physical_cpus);
        EXPECT_EQ(read_u32(reply, info + 32), emu->system_info.logical_cpus);
        EXPECT_EQ(read_u64(reply, info + 40), emu->system_info.memory_size);
    }

    TEST(MigHost, HostInfoTrimsTheReplyToTheRequestedCount)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.host_self, 200, macos_test::ndr_body({flavor::host_basic_info, 4}), 320);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(read_u32(reply, 36), 4u);
        EXPECT_EQ(header.size, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4 + 4 + 16)
            << "an over-large fixed reply is rejected by the caller's own size check";
    }

    TEST(MigHost, AnUnknownFlavorFailsWithoutCorruptingTheReply)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.host_self, 200, macos_test::ndr_body({0x4242, 12}), 320);

        EXPECT_EQ(read_msg_header(reply).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::invalid_argument);
    }

    TEST(MigHost, ClockServiceReplyIsAComplexPortDescriptor)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.host_self, 206, macos_test::ndr_body({SYSTEM_CLOCK}), 48);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 306);
        EXPECT_EQ(header.size, 40u) << "the measured reply size";
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 1u);

        const auto descriptor = read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.type, descriptor_type::port);
        EXPECT_EQ(descriptor.disposition, disposition::move_send)
            << "a descriptor in a received message names the right as received, never as make_send";
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, kernel_object_kind::clock);
    }

    TEST(MigHost, HostSpecialPortAnswersOnTheHostPortOnly)
    {
        const auto emu = macos_test::make_emulator();

        const auto good = macos_test::send_mig_call(*emu, emu->mach.host_self, 412, macos_test::ndr_body({0, host_special_port::priv}), 48);
        EXPECT_EQ(read_msg_header(good).size, 40u);
        EXPECT_NE(read_msg_header(good).bits & BITS_COMPLEX, 0u);

        const auto bad = macos_test::send_mig_call(*emu, emu->mach.task_self, 412, macos_test::ndr_body({0, host_special_port::priv}), 48);
        EXPECT_EQ(read_msg_header(bad).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(bad, 32)), mig_error::bad_id);
    }

    TEST(MigTask, TaskInfoAuditTokenCarriesTheProcessIdentity)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3405,
                                                     macos_test::ndr_body({flavor::task_audit_token, flavor::task_audit_token_count}), 424);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 3505);
        EXPECT_EQ(header.size, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4 + 4 + 32);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_EQ(read_u32(reply, 36), 8u);
        EXPECT_EQ(read_u32(reply, 40 + 4), emu->process.euid);
        EXPECT_EQ(read_u32(reply, 40 + 8), emu->process.egid);
        EXPECT_EQ(read_u32(reply, 40 + 20), emu->process.pid);

        // The last two words are not decoration: libxpc's bootstrap_look_up3 reads this token and calls
        // its fatal path when either the pid or the pid version is clear.
        EXPECT_EQ(read_u32(reply, 40 + 24), emu->process.audit_session_id);
        EXPECT_EQ(read_u32(reply, 40 + 28), emu->process.pid_version);
        EXPECT_NE(emu->process.audit_session_id, 0u);
        EXPECT_NE(emu->process.pid_version, 0u);
    }

    TEST(MigTask, TaskInfoAnswersDyldInfoWithFiveWords)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3405,
                                                     macos_test::ndr_body({flavor::task_dyld_info, flavor::task_dyld_info_count}), 424);

        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_EQ(read_u32(reply, 36), 5u);
        EXPECT_EQ(read_msg_header(reply).size, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4 + 4 + 20);
    }

    TEST(MigTask, GetSpecialPortHandsOutTheBootstrapPort)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3409, macos_test::ndr_body({task_special_port::bootstrap}), 48);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 3509);
        EXPECT_EQ(header.size, 40u);
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);

        const auto descriptor = read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.name, emu->mach.bootstrap) << "this reply is the only way the guest learns the bootstrap port's name";
        EXPECT_EQ(descriptor.disposition, disposition::move_send)
            << "a descriptor in a received message names the right as received, never as make_send";
    }

    TEST(MigTask, SetSpecialPortStoresTheDebugControlPort)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        std::vector<uint8_t> body(MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + 4, 0);
        write_u32(body, 0, 1);
        write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE),
                              {.name = port, .disposition = disposition::copy_send, .type = descriptor_type::port});
        std::ranges::copy(NDR_RECORD, body.begin() + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);
        write_u32(body, MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE, task_special_port::debug_control);

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3410, body, 44, true);

        EXPECT_EQ(read_msg_header(reply).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_EQ(emu->mach.get_task_special_port(task_special_port::debug_control), port);
    }

    TEST(MigTask, SemaphoreCreateReturnsALiveSemaphorePort)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 3418, macos_test::ndr_body({0, 1}), 48);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 3518);
        EXPECT_EQ(header.size, 40u);
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);

        const auto descriptor = read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE));
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, kernel_object_kind::semaphore);
        ASSERT_NE(emu->mach.find_semaphore(descriptor.name), nullptr);
        EXPECT_EQ(emu->mach.find_semaphore(descriptor.name)->value, 1);
    }

    std::vector<uint8_t> vm_map_body(const uint64_t address, const uint64_t size, const uint32_t flags, const uint32_t cur_protection)
    {
        std::vector<uint8_t> body(76, 0);
        write_u32(body, 0, 1);
        write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE),
                              {.name = PORT_NULL, .disposition = disposition::copy_send, .type = descriptor_type::port});
        std::ranges::copy(NDR_RECORD, body.begin() + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);
        write_u64(body, 24, address);
        write_u64(body, 32, size);
        write_u64(body, 40, 0);
        write_u32(body, 48, flags);
        write_u64(body, 52, 0);
        write_u32(body, 60, 0);
        write_u32(body, 64, cur_protection);
        write_u32(body, 68, cur_protection);
        write_u32(body, 72, 1);
        return body;
    }

    TEST(MigVm, VmMapAnywhereAllocatesAndReportsTheAddress)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 4811, vm_map_body(0, sogen::MACOS_PAGE_SIZE, 1, 3), 52, true);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 4911);
        EXPECT_EQ(header.size, 44u) << "the measured reply size for mach_vm_map";
        EXPECT_EQ(header.bits & BITS_COMPLEX, 0u) << "the reply is simple even though the request is complex";
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);

        const auto address = read_u64(reply, 36);
        EXPECT_NE(address, 0u);
        EXPECT_EQ(address % sogen::MACOS_PAGE_SIZE, 0u);
        ASSERT_TRUE(emu->memory.get_region_info(address).has_value());
        EXPECT_EQ(emu->memory.get_region_info(address)->permissions, sogen::memory_permission::read_write);
    }

    TEST(MigVm, VmMapRequestSizeMatchesTheMeasuredWireSize)
    {
        EXPECT_EQ(MSG_HEADER_SIZE + vm_map_body(0, 0, 0, 0).size(), 100u);
    }

    TEST(MigVm, DeferredReclamationBufferAllocateReturnsAnAddressAndAPeriod)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 4822, macos_test::ndr_body({0x1000, 0x4000}), 60);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 4922);
        EXPECT_EQ(header.size, 52u) << "the measured reply size";
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_NE(read_u64(reply, 36), 0u);
    }

    namespace
    {
        std::vector<uint8_t> make_vm_map_body(const port_name_t object, const uint64_t size)
        {
            std::vector<uint8_t> body(MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + 48, 0);
            write_u32(body, 0, 1);
            write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE),
                                  {.name = object, .disposition = disposition::copy_send, .type = descriptor_type::port});
            std::ranges::copy(NDR_RECORD, body.begin() + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);

            const auto args = MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE;
            write_u64(body, args + 8, size);
            write_u32(body, args + 24, 1);
            write_u32(body, args + 40, 3);
            return body;
        }
    }

    // sogen has one address space, so an entry cannot be mapped a second time somewhere else: mapping one
    // hands back the range it was made over. Answering with fresh zeroes instead would be a silent wrong
    // answer -- the guest would read zeroes where it expected the shared page's contents.
    TEST(MigVm, VmMapOfAMemoryEntryHandsBackTheRangeItNames)
    {
        const auto emu = macos_test::make_emulator();

        const uint64_t backing = 0x360000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(backing, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const auto object = emu->mach.create_memory_entry(backing, sogen::MACOS_PAGE_SIZE);

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 4811, make_vm_map_body(object, sogen::MACOS_PAGE_SIZE), 64, true);

        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE + NDR_RECORD_SIZE), static_cast<uint32_t>(kr::success));
        EXPECT_EQ(read_u64(reply, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), backing);
    }

    TEST(MigVm, VmMapRefusesAMemoryEntryNoRoutineMade)
    {
        const auto emu = macos_test::make_emulator();
        const auto object = emu->mach.ports.allocate_receive_right();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 4811, make_vm_map_body(object, sogen::MACOS_PAGE_SIZE), 64, true);

        EXPECT_EQ(read_msg_header(reply).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::invalid_argument);
    }

    TEST(MigVm, VmMapRefusesMoreThanAMemoryEntryBacks)
    {
        const auto emu = macos_test::make_emulator();

        const uint64_t backing = 0x360000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(backing, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        const auto object = emu->mach.create_memory_entry(backing, sogen::MACOS_PAGE_SIZE);

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 4811, make_vm_map_body(object, sogen::MACOS_PAGE_SIZE * 2), 64, true);

        EXPECT_EQ(read_msg_header(reply).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::invalid_argument);
    }
}

namespace
{
    TEST(MigTaskRestartable, RegisterSucceedsWithoutParsingTheRanges)
    {
        const auto emu = macos_test::make_emulator();

        std::vector<uint8_t> body(NDR_RECORD_SIZE + 4 + 5 * 16, 0);
        std::ranges::copy(NDR_RECORD, body.begin());
        write_u32(body, NDR_RECORD_SIZE, 5);

        EXPECT_EQ(MSG_HEADER_SIZE + body.size(), 116u) << "the measured send size for routine 8000";

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 8000, body, 44);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 8100);
        EXPECT_EQ(header.size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
    }

    TEST(MigTaskRestartable, SynchronizeAcceptsABareHeaderRequest)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 8001, {}, 44);
        const auto header = read_msg_header(reply);

        EXPECT_EQ(header.id, 8101);
        EXPECT_EQ(header.size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success)
            << "a no-argument request has no NDR record; a decoder that demands one returns MIG_BAD_ARGUMENTS here";
    }

    TEST(MigTaskRestartable, TheTwoRoutinesAreDistinctTableEntriesOnTheTaskPort)
    {
        const auto& table = kernel_mig_servers();

        ASSERT_NE(table.find(kernel_object_kind::task, 8000), nullptr);
        ASSERT_NE(table.find(kernel_object_kind::task, 8001), nullptr);
        EXPECT_NE(table.name_of(kernel_object_kind::task, 8000), table.name_of(kernel_object_kind::task, 8001));
        EXPECT_FALSE(table.name_of(kernel_object_kind::task, 8000).empty());
        EXPECT_EQ(table.find(kernel_object_kind::host, 8000), nullptr) << "task_restartable is a task-port subsystem";
    }

    // MIG's own send path asks the kernel for a send-once right on the reply port before using it.
    // Leaving the routine unimplemented made libSystem retry the call from inside pthread's
    // initialisation until the guest stack ran out -- 40,327 mach_msg2 calls into the run.
    TEST(MigPort, ExtractRightHandsBackTheRightTheCallerAskedFor)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3215, macos_test::ndr_body({port, disposition::make_send_once}), 64);

        const auto header = read_msg_header(reply);
        ASSERT_NE(header.bits & BITS_COMPLEX, 0u) << "the right travels as a descriptor, not as a scalar";
        EXPECT_EQ(header.id, 3215 + 100);

        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 1u) << "one descriptor";

        port_descriptor descriptor{};
        std::memcpy(&descriptor.name, reply.data() + MSG_HEADER_SIZE + MSG_BODY_SIZE, sizeof(descriptor.name));

        // A *new* name, not the receive port's own. Handing back the name the caller already holds
        // looks like success and is not: it decides the extraction did not happen and asks again.
        EXPECT_NE(descriptor.name, port);
        EXPECT_NE(descriptor.name, PORT_NULL);
        EXPECT_EQ(emu->mach.ports.destination_of(descriptor.name), emu->mach.ports.find(port))
            << "the new name has to lead back to the port it was extracted from";

        EXPECT_EQ(reply[MSG_HEADER_SIZE + MSG_BODY_SIZE + 10], disposition::move_send_once)
            << "a descriptor in a received message names the right as received, not as it was asked for";
    }

    // A disposition names one thing when a message is sent and another once it has arrived: make- and
    // copy- are instructions to the kernel about a right to create, and a message being *received* can
    // only carry the right itself. Echoing the send-side form back is not cosmetic -- the guest's own
    // message-teardown path reads this byte, sees a make- form on an arriving message, and tries to
    // extract a right from it, which is another message with the same reply. Real dyld span 40,327
    // mach_msg2 calls on exactly that before the stack ran out.
    TEST(MigDispatch, AReplyCarriesTheReceivedDispositionNotTheRequestedOne)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3409, macos_test::ndr_body({task_special_port::bootstrap}), 64);

        const auto header = read_msg_header(reply);
        EXPECT_EQ((header.bits >> 8) & 0xFF, disposition::move_send_once)
            << "send_mig_call asks for make_send_once on the reply port, and a received message says move";
        EXPECT_EQ(header.bits & 0xFF, 0u) << "no right travels back, so the remote slot is empty";

        // The error path builds its own header, so it has to translate too.
        const auto refused = macos_test::send_mig_call(*emu, emu->mach.task_self, 31337, macos_test::ndr_body({}), 64);
        EXPECT_EQ((read_msg_header(refused).bits >> 8) & 0xFF, disposition::move_send_once);
        EXPECT_EQ(read_msg_header(refused).bits & BITS_COMPLEX, 0u) << "an error reply carries no descriptor";
    }

    TEST(MigPort, ExtractRightRefusesAnUnknownNameAndAnUnknownRight)
    {
        const auto emu = macos_test::make_emulator();
        const auto port = emu->mach.ports.allocate_receive_right();

        const auto missing =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3215, macos_test::ndr_body({0xDEAD, disposition::make_send_once}), 64);
        EXPECT_EQ(read_msg_header(missing).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(missing, 32)), kr::invalid_name);

        // move_receive would hand the caller the port itself, which is not a right this routine grants.
        const auto wrong =
            macos_test::send_mig_call(*emu, emu->mach.task_self, 3215, macos_test::ndr_body({port, disposition::move_receive}), 64);
        EXPECT_EQ(read_msg_header(wrong).size, MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(wrong, 32)), kr::invalid_value);
    }

    // An unimplemented MIG routine used to be silent, unlike an unimplemented syscall, so a guest that
    // retried one spun with nothing naming the routine responsible.
    TEST(MigDispatch, AnUnimplementedRoutineIsNamedNotJustRefused)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(*emu, emu->mach.task_self, 31337, macos_test::ndr_body({}), 64);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), mig_error::bad_id);
    }

    // libpthread asks for this before anything else runs and treats a refusal as fatal: it crashes with
    // "BUG IN LIBPTHREAD: host_info() failed" rather than degrading. The values are measured from a real
    // host -- they describe the kernel's scheduler, not any particular machine.
    TEST(MigHost, HostInfoAnswersThePriorityFlavourLibpthreadRequires)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = macos_test::send_mig_call(
            *emu, emu->mach.host_self, 200, macos_test::ndr_body({flavor::host_priority_info, flavor::host_priority_info_count}), 128);

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, 300);
        EXPECT_EQ(header.bits & BITS_COMPLEX, 0u) << "a counted reply carries no descriptor";
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);
        EXPECT_EQ(read_u32(reply, 36), flavor::host_priority_info_count);

        // MIG sizes a counted reply from the count it declares, so the two have to agree exactly.
        EXPECT_EQ(header.size, 40 + flavor::host_priority_info_count * sizeof(uint32_t));

        const std::array<uint32_t, 8> expected{80, 80, 64, 31, 0, 0, 0, 79};
        for (size_t i = 0; i < expected.size(); ++i)
        {
            EXPECT_EQ(read_u32(reply, 40 + i * sizeof(uint32_t)), expected[i]) << "priority word " << i;
        }
    }

    TEST(MigHost, HostInfoTruncatesToTheCountTheCallerAskedFor)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply =
            macos_test::send_mig_call(*emu, emu->mach.host_self, 200, macos_test::ndr_body({flavor::host_priority_info, 3}), 128);

        EXPECT_EQ(read_u32(reply, 36), 3u);
        EXPECT_EQ(read_msg_header(reply).size, 40 + 3 * sizeof(uint32_t)) << "a short count means a short reply";
    }
}
