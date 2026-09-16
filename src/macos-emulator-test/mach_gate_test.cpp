#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mig_kernel_servers.hpp>

#include <algorithm>
#include <ranges>
#include <cstring>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    constexpr uint64_t gate_code_base = 0x180000000ULL;
    constexpr uint64_t gate_msg_base = 0x380000000ULL;
    constexpr uint64_t GATE_MAGIC = 0x5A0'6E14'0BEE'F00DULL;

    constexpr size_t VM_MAP_ARGS_SIZE = 48;
    constexpr uint32_t VM_FLAGS_ANYWHERE = 1;
    constexpr int32_t VM_PROT_READ_WRITE = 3;

    std::vector<uint8_t> vm_map_request(const port_name_t task_port, const port_name_t reply_port, const uint64_t size)
    {
        constexpr size_t args = MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE;
        const auto send_size = static_cast<uint32_t>(MSG_HEADER_SIZE + args + VM_MAP_ARGS_SIZE);

        std::vector<uint8_t> message(send_size, 0);
        write_msg_header(message, {.bits = BITS_COMPLEX | make_bits(disposition::copy_send, disposition::make_send_once),
                                   .size = send_size,
                                   .remote_port = task_port,
                                   .local_port = reply_port,
                                   .voucher_port = 0,
                                   .id = 4811});

        write_u32(message, MSG_HEADER_SIZE, 1);
        write_port_descriptor(std::span{message}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE),
                              {.name = PORT_NULL, .disposition = disposition::copy_send, .type = descriptor_type::port});
        std::ranges::copy(NDR_RECORD, message.begin() + MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);

        write_u64(message, MSG_HEADER_SIZE + args + 8, size);
        write_u32(message, MSG_HEADER_SIZE + args + 24, VM_FLAGS_ANYWHERE);
        write_u32(message, MSG_HEADER_SIZE + args + 40, static_cast<uint32_t>(VM_PROT_READ_WRITE));

        return message;
    }

    // Stage 4's gate. A guest program asks the kernel for memory over Mach IPC and then *uses the answer*:
    // it writes a magic value to an address that exists nowhere in this test until the reply carries it
    // back. Passing means the whole chain works from the guest side -- the mach_msg2 trap, the message
    // decode, the port-kind dispatch, the MIG server, the reply encoding, and the guest's own read of it.
    TEST(MachGate, AGuestAllocatesMemoryOverMachIpcAndWritesToIt)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply_port = emu->mach.make_special_reply_port(sogen::MACH_MAIN_THREAD_ID);
        const auto request = vm_map_request(emu->mach.task_self, reply_port, sogen::MACOS_PAGE_SIZE);

        ASSERT_TRUE(emu->memory.allocate_memory(gate_msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.write_memory(gate_msg_base, request.data(), request.size());

        macos_test::mach_msg2_args args{};
        args.buffer = gate_msg_base;
        args.options = msg_option::send_msg | msg_option::rcv_msg;
        args.bits = read_u32(request, 0);
        args.send_size = static_cast<uint32_t>(request.size());
        args.remote_port = emu->mach.task_self;
        args.local_port = reply_port;
        args.id = 4811;
        args.descriptor_count = 1;
        args.rcv_name = reply_port;
        args.rcv_size = 64;

        auto words = macos_test::mach_msg2_words(args);

        macos_test::load_x(words, 9, gate_msg_base);
        words.push_back(0xF842412B); // ldur x11, [x9, #36]   -- the address the kernel chose
        macos_test::load_x(words, 12, GATE_MAGIC);
        words.push_back(0xF900016C); // str x12, [x11]
        words.push_back(0xB9402120); // ldr w0, [x9, #32]     -- exit status is the kern_return
        words.push_back(macos_test::movz_x(16, 1, 0));
        words.push_back(0xD4001001); // svc #0x80

        macos_test::write_guest_code(*emu, gate_code_base, words);
        emu->start(words.size() + 8);

        ASSERT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, 0) << "the guest exits with the kern_return it was given";

        std::vector<uint8_t> reply(64, 0);
        emu->memory.read_memory(gate_msg_base, reply.data(), reply.size());

        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, 4811 + 100) << "MIG replies are the request id plus 100";
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::success);

        uint64_t allocated{};
        std::memcpy(&allocated, reply.data() + 36, sizeof(allocated));
        ASSERT_NE(allocated, 0u);

        uint64_t stored{};
        ASSERT_TRUE(emu->memory.try_read_memory(allocated, &stored, sizeof(stored)))
            << "the kernel said it mapped " << std::hex << allocated;
        EXPECT_EQ(stored, GATE_MAGIC) << "the guest wrote through an address it only learned from the reply";
    }

    TEST(MachGate, AGuestThatAsksForAnImpossibleSizeGetsAnErrorItCanSee)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply_port = emu->mach.make_special_reply_port(sogen::MACH_MAIN_THREAD_ID);
        const auto request = vm_map_request(emu->mach.task_self, reply_port, 0);

        ASSERT_TRUE(emu->memory.allocate_memory(gate_msg_base, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        emu->memory.write_memory(gate_msg_base, request.data(), request.size());

        macos_test::mach_msg2_args args{};
        args.buffer = gate_msg_base;
        args.options = msg_option::send_msg | msg_option::rcv_msg;
        args.bits = read_u32(request, 0);
        args.send_size = static_cast<uint32_t>(request.size());
        args.remote_port = emu->mach.task_self;
        args.local_port = reply_port;
        args.id = 4811;
        args.descriptor_count = 1;
        args.rcv_name = reply_port;
        args.rcv_size = 64;

        auto words = macos_test::mach_msg2_words(args);
        macos_test::load_x(words, 9, gate_msg_base);
        words.push_back(0xB9402120); // ldr w0, [x9, #32]
        words.push_back(macos_test::movz_x(16, 1, 0));
        words.push_back(0xD4001001); // svc #0x80

        macos_test::write_guest_code(*emu, gate_code_base, words);
        emu->start(words.size() + 8);

        ASSERT_EQ(emu->last_stop_reason(), sogen::stop_reason::normal_exit) << emu->last_stop_detail();
        ASSERT_TRUE(emu->process.exit_status.has_value());
        EXPECT_EQ(*emu->process.exit_status, kr::invalid_argument)
            << "a refusal has to reach the guest as a kern_return, not as a stopped emulator";
    }
}
