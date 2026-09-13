#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <algorithm>
#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    // Where AppKit's window path lands. -[NSView setLayer:] sends NS_setView:, and
    // class_getMethodImplementation(CALayer, NS_setView:) is a runtime address in no image -- measured on
    // the host, 0x100090020 -- because AppKit adds the method with imp_implementationWithBlock. libobjc
    // builds those IMPs by mach_vm_remapping the __text page of /usr/lib/libobjc-trampolines.dylib over
    // the second page of a two-page vm_allocate, so every block IMP any framework installs is whatever
    // that remap produced. A remap that lands the wrong bytes is a SIGILL or a PAC failure inside
    // objc_msgSend, a very long way from the routine that got it wrong.
    //
    // mach_vm_remap with VM_FLAGS_OVERWRITE over a range the caller already owns, which is exactly the
    // shape objc4's _allocateTrampolinesAndData uses: vm_allocate two pages, then remap the template
    // over the second one.
    //
    // The wire layout is MIG's, which packs to 4 and inserts no padding after the int flags field:
    // target_address 0, size 8, mask 16, flags 24, src_address 28, copy 36, inheritance 40 -- 44 bytes.
    // 24 header + 4 body + 12 port descriptor + 8 NDR + 44 is the 92-byte request libobjc sends, which
    // is what says the block is 44 and not 48.
    constexpr size_t VM_REMAP_ARGS_SIZE = 44;
    constexpr size_t VM_REMAP_REQUEST_SIZE = MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + VM_REMAP_ARGS_SIZE;

    std::vector<uint8_t> vm_remap_request(const port_name_t task_port, const port_name_t reply_port, const uint64_t target,
                                          const uint64_t source, const uint64_t size)
    {
        constexpr uint32_t VM_FLAGS_OVERWRITE = 0x4000;
        constexpr uint32_t VM_INHERIT_SHARE = 0;
        constexpr size_t PREFIX = MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE;

        std::vector<uint8_t> message(VM_REMAP_REQUEST_SIZE, 0);
        write_msg_header(message, {.bits = BITS_COMPLEX | make_bits(disposition::copy_send, disposition::make_send_once),
                                   .size = static_cast<uint32_t>(message.size()),
                                   .remote_port = task_port,
                                   .local_port = reply_port,
                                   .voucher_port = 0,
                                   .id = 4813});

        write_u32(message, MSG_HEADER_SIZE, 1);
        write_port_descriptor(std::span{message}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE),
                              {.name = task_port, .disposition = disposition::copy_send, .type = descriptor_type::port});
        std::ranges::copy(NDR_RECORD, message.begin() + MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);

        write_u64(message, PREFIX + 0, target);
        write_u64(message, PREFIX + 8, size);
        write_u64(message, PREFIX + 16, 0);
        write_u32(message, PREFIX + 24, VM_FLAGS_OVERWRITE);
        write_u64(message, PREFIX + 28, source);
        write_u32(message, PREFIX + 36, 1);
        write_u32(message, PREFIX + 40, VM_INHERIT_SHARE);
        return message;
    }

    TEST(ObjcTrampoline, VmRemapCopiesTheSourcePageOverAFixedTarget)
    {
        constexpr uint64_t source_base = 0x200000000ULL;
        constexpr uint64_t pair_base = 0x210000000ULL;

        const auto emu = macos_test::make_emulator();
        const auto page = static_cast<size_t>(sogen::MACOS_PAGE_SIZE);

        ASSERT_TRUE(emu->memory.allocate_memory(source_base, page, sogen::memory_permission::read_exec));
        ASSERT_TRUE(emu->memory.allocate_memory(pair_base, page * 2, sogen::memory_permission::read_write));

        std::vector<uint8_t> pattern(page, 0);
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            pattern[i] = static_cast<uint8_t>(i * 7 + 1);
        }
        ASSERT_TRUE(emu->memory.try_write_memory(source_base, pattern.data(), pattern.size()));

        const auto reply_port = emu->mach.make_special_reply_port(sogen::MACH_MAIN_THREAD_ID);
        const auto request =
            vm_remap_request(emu->mach.task_self, reply_port, pair_base + sogen::MACOS_PAGE_SIZE, source_base, sogen::MACOS_PAGE_SIZE);
        ASSERT_EQ(request.size(), 92u) << "the size libobjc's vm_remap of the trampoline template is measured to send";

        constexpr uint64_t msg_base = 0x380000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(msg_base, page, sogen::memory_permission::read_write));
        emu->memory.write_memory(msg_base, request.data(), request.size());

        macos_test::mach_msg2_args args{};
        args.buffer = msg_base;
        args.options = msg_option::send_msg | msg_option::rcv_msg;
        args.bits = read_u32(request, 0);
        args.send_size = static_cast<uint32_t>(request.size());
        args.remote_port = emu->mach.task_self;
        args.local_port = reply_port;
        args.id = 4813;
        args.descriptor_count = 1;
        args.rcv_name = reply_port;
        args.rcv_size = 128;

        constexpr uint64_t code_base = 0x390000000ULL;
        ASSERT_TRUE(emu->memory.allocate_memory(code_base, page, sogen::memory_permission::all));
        const auto words = macos_test::mach_msg2_words(args);
        macos_test::write_guest_code(*emu, code_base, words);
        emu->start(words.size());

        std::vector<uint8_t> copied(page, 0);
        ASSERT_TRUE(emu->memory.try_read_memory(pair_base + sogen::MACOS_PAGE_SIZE, copied.data(), copied.size()));
        EXPECT_EQ(copied, pattern) << "the remapped view does not hold the source bytes";
    }
}
