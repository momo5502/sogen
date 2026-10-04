#include <gtest/gtest.h>

#include <mach/mach_types.hpp>

#include <algorithm>
#include <limits>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace) // NOLINT(google-build-using-namespace): the tree's own namespace,
                                 // pulled into a test-local anonymous namespace

    TEST(MachTypes, StructureSizesMatchDarwin)
    {
        static_assert(MSG_HEADER_SIZE == 24);
        static_assert(MSG_BODY_SIZE == 4);
        static_assert(PORT_DESCRIPTOR_SIZE == 12);
        static_assert(OOL_DESCRIPTOR_SIZE == 16);
        static_assert(NDR_RECORD_SIZE == 8);
        static_assert(MIG_REPLY_ERROR_SIZE == 36);
        static_assert(TRAILER_SIZE == 8);
        static_assert(MSG_VECTOR_ELEMENT_SIZE == 24);
        static_assert(MSG_AUX_HEADER_SIZE == 8);
        static_assert(MIG_REPLY_ERROR_SIZE == MSG_HEADER_SIZE + NDR_RECORD_SIZE + sizeof(kern_return_t));
        SUCCEED();
    }

    TEST(MachTypes, HeaderRoundTripsAtTheDocumentedOffsets)
    {
        std::array<uint8_t, MSG_HEADER_SIZE> buffer{};
        const msg_header header{
            .bits = 0x80001513u,
            .size = 100,
            .remote_port = 0x203,
            .local_port = 0x70b,
            .voucher_port = 0,
            .id = 4811,
        };

        write_msg_header(buffer, header);

        EXPECT_EQ(read_u32(buffer, 0), 0x80001513u);
        EXPECT_EQ(read_u32(buffer, 4), 100u);
        EXPECT_EQ(read_u32(buffer, 8), 0x203u);
        EXPECT_EQ(read_u32(buffer, 12), 0x70bu);
        EXPECT_EQ(read_u32(buffer, 16), 0u);
        EXPECT_EQ(read_u32(buffer, 20), 4811u);

        const auto decoded = read_msg_header(buffer);
        EXPECT_EQ(decoded.bits, header.bits);
        EXPECT_EQ(decoded.id, header.id);
        EXPECT_EQ(decoded.local_port, header.local_port);
    }

    TEST(MachTypes, HeaderDecodesEveryFieldIntoItsOwnSlot)
    {
        std::array<uint8_t, MSG_HEADER_SIZE> buffer{};
        const msg_header header{
            .bits = 0x11111111u,
            .size = 0x22222222u,
            .remote_port = 0x33333333u,
            .local_port = 0x44444444u,
            .voucher_port = 0x55555555u,
            .id = 0x66666666,
        };

        write_msg_header(buffer, header);
        const auto decoded = read_msg_header(buffer);

        EXPECT_EQ(decoded.bits, 0x11111111u);
        EXPECT_EQ(decoded.size, 0x22222222u);
        EXPECT_EQ(decoded.remote_port, 0x33333333u);
        EXPECT_EQ(decoded.local_port, 0x44444444u);
        EXPECT_EQ(decoded.voucher_port, 0x55555555u);
        EXPECT_EQ(decoded.id, 0x66666666);
    }

    TEST(MachTypes, HeaderBytesAreLittleEndian)
    {
        std::array<uint8_t, MSG_HEADER_SIZE> buffer{};
        write_msg_header(buffer, {
                                     .bits = 0x80001513u,
                                     .size = 100,
                                     .remote_port = 0x203,
                                     .local_port = 0x70b,
                                     .voucher_port = 0,
                                     .id = 4811,
                                 });

        const std::array<uint8_t, MSG_HEADER_SIZE> expected{0x13, 0x15, 0x00, 0x80, 0x64, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00,
                                                            0x0b, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcb, 0x12, 0x00, 0x00};
        EXPECT_EQ(buffer, expected);
    }

    TEST(MachTypes, NegativeIdSurvivesTheRoundTrip)
    {
        std::array<uint8_t, MSG_HEADER_SIZE> buffer{};
        write_msg_header(buffer, {.id = mig_error::bad_id});

        EXPECT_EQ(read_u32(buffer, 20), 0xFFFFFED1u);
        EXPECT_EQ(read_msg_header(buffer).id, -303);
    }

    TEST(MachTypes, PortDescriptorMatchesTheMeasuredByteImage)
    {
        std::array<uint8_t, PORT_DESCRIPTOR_SIZE> buffer{};
        write_port_descriptor(buffer, {.name = 0xAABBCCDD, .disposition = disposition::make_send, .type = descriptor_type::port});

        const std::array<uint8_t, PORT_DESCRIPTOR_SIZE> expected{0xdd, 0xcc, 0xbb, 0xaa, 0, 0, 0, 0, 0, 0, 0x14, 0x00};
        EXPECT_EQ(buffer, expected);

        const auto decoded = read_port_descriptor(buffer);
        EXPECT_EQ(decoded.name, 0xAABBCCDDu);
        EXPECT_EQ(decoded.disposition, disposition::make_send);
        EXPECT_EQ(decoded.type, descriptor_type::port);
    }

    TEST(MachTypes, PortDescriptorKeepsDispositionAndTypeApart)
    {
        std::array<uint8_t, PORT_DESCRIPTOR_SIZE> buffer{};
        write_port_descriptor(buffer, {.name = 0x1234, .disposition = disposition::move_receive, .type = descriptor_type::guarded_port});

        EXPECT_EQ(buffer[10], disposition::move_receive);
        EXPECT_EQ(buffer[11], descriptor_type::guarded_port);

        const auto decoded = read_port_descriptor(buffer);
        EXPECT_EQ(decoded.disposition, disposition::move_receive);
        EXPECT_EQ(decoded.type, descriptor_type::guarded_port);
    }

    TEST(MachTypes, OolDescriptorMatchesTheMeasuredByteImage)
    {
        std::array<uint8_t, OOL_DESCRIPTOR_SIZE> buffer{};
        write_ool_descriptor(buffer, {.address = 0x1122334455667788ull,
                                      .size = 0x99AABBCCu,
                                      .deallocate = 1,
                                      .copy = copy_option::virtual_copy,
                                      .type = descriptor_type::ool});

        const std::array<uint8_t, OOL_DESCRIPTOR_SIZE> expected{0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                                                                0x01, 0x01, 0x00, 0x01, 0xcc, 0xbb, 0xaa, 0x99};
        EXPECT_EQ(buffer, expected);

        const auto decoded = read_ool_descriptor(buffer);
        EXPECT_EQ(decoded.address, 0x1122334455667788ull);
        EXPECT_EQ(decoded.size, 0x99AABBCCu);
        EXPECT_EQ(decoded.deallocate, 1);
        EXPECT_EQ(decoded.copy, copy_option::virtual_copy);
        EXPECT_EQ(decoded.type, descriptor_type::ool);
    }

    // The two elements of a live __CFRunLoopServiceMachPort call and of an _xpc_connection_check_in,
    // captured 2026-08-28 from the trap's x0 buffer on the host.
    TEST(MachTypes, VectorElementMatchesTheMeasuredByteImage)
    {
        constexpr std::array<uint8_t, 2 * MSG_VECTOR_ELEMENT_SIZE> measured{
            0x78, 0xa2, 0x5a, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0xcd, 0xdf, 0x6f, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
        };

        const auto message = read_msg_vector_element(std::span<const uint8_t>{measured}.first(MSG_VECTOR_ELEMENT_SIZE));
        EXPECT_EQ(message.data, 0x1005aa278ull);
        EXPECT_EQ(message.rcv_address, 0u);
        EXPECT_EQ(message.send_size, 0x34u);
        EXPECT_EQ(message.rcv_size, 0u);

        const auto aux = read_msg_vector_element(std::span<const uint8_t>{measured}.last(MSG_VECTOR_ELEMENT_SIZE));
        EXPECT_EQ(aux.data, 0x16fdfcd18ull);
        EXPECT_EQ(aux.send_size, 0x28u) << "libsyscall's aux is an 8-byte header and 32 bytes of payload";
        EXPECT_EQ(aux.rcv_size, 0x80u) << "LIBSYSCALL_MSGV_AUX_MAX_SIZE";

        std::array<uint8_t, 2 * MSG_VECTOR_ELEMENT_SIZE> rebuilt{};
        write_msg_vector_element(std::span{rebuilt}.first(MSG_VECTOR_ELEMENT_SIZE), message);
        write_msg_vector_element(std::span{rebuilt}.last(MSG_VECTOR_ELEMENT_SIZE), aux);
        EXPECT_EQ(rebuilt, measured);
    }

    TEST(MachTypes, EveryDescriptorShapeIsSixteenBytesExceptThePortOne)
    {
        EXPECT_EQ(descriptor_size(descriptor_type::port), PORT_DESCRIPTOR_SIZE);
        EXPECT_EQ(descriptor_size(descriptor_type::ool), OOL_DESCRIPTOR_SIZE);
        EXPECT_EQ(descriptor_size(descriptor_type::ool_ports), OOL_DESCRIPTOR_SIZE);
        EXPECT_EQ(descriptor_size(descriptor_type::ool_volatile), OOL_DESCRIPTOR_SIZE);
        EXPECT_EQ(descriptor_size(descriptor_type::guarded_port), OOL_DESCRIPTOR_SIZE);
    }

    TEST(MachTypes, BitsDecodeTheObservedMigShape)
    {
        static_assert(make_bits(disposition::copy_send, disposition::make_send_once) == 0x1513u);
        EXPECT_EQ(remote_disposition(0x80001513u), disposition::copy_send);
        EXPECT_EQ(local_disposition(0x80001513u), disposition::make_send_once);
        EXPECT_EQ(voucher_disposition(0x00131513u), disposition::copy_send);
        EXPECT_EQ(voucher_disposition(0x00001513u), 0u);
        EXPECT_NE(0x80001513u & BITS_COMPLEX, 0u);
        EXPECT_EQ(0x00001513u & BITS_COMPLEX, 0u);
    }

    TEST(MachTypes, DispositionFieldsAreFiveBitsWideNotEight)
    {
        EXPECT_EQ(remote_disposition(0x800015F3u), disposition::copy_send);
        EXPECT_EQ(local_disposition(0x8000F513u), disposition::make_send_once);
        EXPECT_EQ(voucher_disposition(0x00F31513u), disposition::copy_send);

        static_assert(make_bits(0xFFu, 0xFFu) == 0x1F1Fu);
        static_assert((BITS_REMOTE_MASK | BITS_LOCAL_MASK | BITS_VOUCHER_MASK | BITS_COMPLEX) == 0x801F1F1Fu);
        SUCCEED();
    }

    TEST(MachTypes, MakeBitsPlacesTheLocalDispositionInTheSecondByte)
    {
        static_assert(make_bits(disposition::move_receive, 0) == 0x0010u);
        static_assert(make_bits(0, disposition::move_receive) == 0x1000u);
        EXPECT_EQ(remote_disposition(make_bits(disposition::move_send, disposition::copy_receive)), disposition::move_send);
        EXPECT_EQ(local_disposition(make_bits(disposition::move_send, disposition::copy_receive)), disposition::copy_receive);
        EXPECT_EQ(voucher_disposition(make_bits(disposition::move_send, disposition::copy_receive)), 0u);
    }

    TEST(MachTypes, PortNamesCarryAGenerationInTheLowByte)
    {
        static_assert(make_port_name(7, 11) == 0x70bu);
        static_assert(port_index(0x70bu) == 7);
        static_assert(port_generation(0x70bu) == 11);
        static_assert(make_port_name(2, 3) == 0x203u);
        static_assert(port_index(0x1e03u) == 0x1e);
        SUCCEED();
    }

    TEST(MachTypes, PortNameEncodingCoversTheWholeRange)
    {
        static_assert(make_port_name(0x00FFFFFFu, 0xFFu) == 0xFFFFFFFFu);
        static_assert(port_index(PORT_DEAD) == 0x00FFFFFFu);
        static_assert(port_generation(PORT_DEAD) == 0xFFu);
        static_assert(port_index(PORT_NULL) == 0u);
        static_assert(port_generation(PORT_NULL) == 0u);
        static_assert(make_port_name(8, 3) == 0x803u);
        static_assert(make_port_name(8, 7) == 0x807u);
        static_assert(make_port_name(8, 3) != make_port_name(8, 7));
        SUCCEED();
    }

    TEST(MachTypes, WordAccessorsAreLittleEndian)
    {
        std::array<uint8_t, 16> buffer{};

        write_u32(buffer, 0, 0xDEADBEEFu);
        EXPECT_EQ(buffer[0], 0xEF);
        EXPECT_EQ(buffer[1], 0xBE);
        EXPECT_EQ(buffer[2], 0xAD);
        EXPECT_EQ(buffer[3], 0xDE);
        EXPECT_EQ(read_u32(buffer, 0), 0xDEADBEEFu);

        write_u64(buffer, 8, 0x0102030405060708ull);
        const std::array<uint8_t, 8> expected{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), buffer.begin() + 8));
        EXPECT_EQ(read_u64(buffer, 8), 0x0102030405060708ull);
        EXPECT_EQ(read_u32(buffer, 8), 0x05060708u);
        EXPECT_EQ(read_u32(buffer, 12), 0x01020304u);
    }

    TEST(MachTypes, WordAccessorsRefuseToRunOffTheEndOfAGuestBuffer)
    {
        std::array<uint8_t, 6> buffer{};
        buffer.fill(0xAB);

        write_u32(buffer, 4, 0xDEADBEEFu);
        write_u64(buffer, 0, 0x1122334455667788ull);

        const std::array<uint8_t, 6> untouched{0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB};
        EXPECT_EQ(buffer, untouched);

        EXPECT_EQ(read_u32(buffer, 4), 0u);
        EXPECT_EQ(read_u32(buffer, 6), 0u);
        EXPECT_EQ(read_u64(buffer, 0), 0u);

        constexpr auto wrapping_offset = std::numeric_limits<size_t>::max() - 1;
        write_u32(buffer, wrapping_offset, 0xDEADBEEFu);
        write_u64(buffer, wrapping_offset, 0xDEADBEEFu);
        EXPECT_EQ(buffer, untouched);
        EXPECT_EQ(read_u32(buffer, wrapping_offset), 0u);
        EXPECT_EQ(read_u64(buffer, wrapping_offset), 0u);

        const auto header = read_msg_header(std::span<const uint8_t>{buffer});
        EXPECT_EQ(header.bits, 0xABABABABu);
        EXPECT_EQ(header.size, 0u);
        EXPECT_EQ(header.remote_port, 0u);
        EXPECT_EQ(header.id, 0);

        const auto descriptor = read_port_descriptor(std::span<const uint8_t>{buffer});
        EXPECT_EQ(descriptor.name, 0xABABABABu);
        EXPECT_EQ(descriptor.disposition, 0);
        EXPECT_EQ(descriptor.type, 0);

        EXPECT_EQ(read_ool_descriptor(std::span<const uint8_t>{buffer}).address, 0u);
    }

    TEST(MachTypes, NdrRecordMatchesTheWireBytes)
    {
        const std::array<uint8_t, 8> expected{0, 0, 0, 0, 1, 0, 0, 0};
        EXPECT_EQ(NDR_RECORD, expected);
    }

    TEST(MachTypes, ReturnCodesAreDarwinsNotLinuxErrnos)
    {
        EXPECT_EQ(kr::failure, 5);
        EXPECT_EQ(kr::invalid_name, 15);
        EXPECT_EQ(kr::not_supported, 46);
        EXPECT_EQ(msgr::send_invalid_dest, 0x10000003u);
        EXPECT_EQ(msgr::rcv_timed_out, 0x10004003u);
        EXPECT_EQ(mig_error::bad_id, -303);
    }

    TEST(MachTypes, EveryReturnCodeMatchesTheHostHeaders)
    {
        EXPECT_EQ(kr::success, 0);
        EXPECT_EQ(kr::invalid_address, 1);
        EXPECT_EQ(kr::protection_failure, 2);
        EXPECT_EQ(kr::no_space, 3);
        EXPECT_EQ(kr::invalid_argument, 4);
        EXPECT_EQ(kr::resource_shortage, 6);
        EXPECT_EQ(kr::not_receiver, 7);
        EXPECT_EQ(kr::no_access, 8);
        EXPECT_EQ(kr::name_exists, 13);
        EXPECT_EQ(kr::invalid_task, 16);
        EXPECT_EQ(kr::invalid_right, 17);
        EXPECT_EQ(kr::invalid_value, 18);
        EXPECT_EQ(kr::urefs_overflow, 19);
        EXPECT_EQ(kr::invalid_capability, 20);
        EXPECT_EQ(kr::right_exists, 21);
        EXPECT_EQ(kr::terminated, 37);
        EXPECT_EQ(kr::operation_timed_out, 49);

        EXPECT_EQ(msgr::success, 0u);
        EXPECT_EQ(msgr::send_invalid_data, 0x10000002u);
        EXPECT_EQ(msgr::send_timed_out, 0x10000004u);
        EXPECT_EQ(msgr::send_msg_too_small, 0x10000008u);
        EXPECT_EQ(msgr::send_invalid_reply, 0x10000009u);
        EXPECT_EQ(msgr::send_invalid_type, 0x1000000fu);
        EXPECT_EQ(msgr::send_invalid_header, 0x10000010u);
        EXPECT_EQ(msgr::rcv_invalid_name, 0x10004002u);
        EXPECT_EQ(msgr::rcv_too_large, 0x10004004u);
        EXPECT_EQ(msgr::rcv_invalid_data, 0x10004008u);
        EXPECT_EQ(msgr::rcv_port_died, 0x10004009u);
        EXPECT_EQ(msgr::rcv_header_error, 0x1000400bu);
        EXPECT_EQ(msgr::rcv_invalid_arguments, 0x10004013u);

        EXPECT_EQ(mig_error::type_error, -300);
        EXPECT_EQ(mig_error::reply_mismatch, -301);
        EXPECT_EQ(mig_error::remote_error, -302);
        EXPECT_EQ(mig_error::bad_arguments, -304);
        EXPECT_EQ(mig_error::array_too_large, -307);
        EXPECT_EQ(mig_error::server_died, -308);
    }

    TEST(MachTypes, DispositionAndDescriptorConstantsMatchTheHostHeaders)
    {
        EXPECT_EQ(disposition::move_receive, 16);
        EXPECT_EQ(disposition::move_send, 17);
        EXPECT_EQ(disposition::move_send_once, 18);
        EXPECT_EQ(disposition::copy_send, 19);
        EXPECT_EQ(disposition::make_send, 20);
        EXPECT_EQ(disposition::make_send_once, 21);
        EXPECT_EQ(disposition::copy_receive, 22);
        EXPECT_EQ(disposition::dispose_receive, 24);
        EXPECT_EQ(disposition::dispose_send, 25);
        EXPECT_EQ(disposition::dispose_send_once, 26);

        EXPECT_EQ(descriptor_type::port, 0);
        EXPECT_EQ(descriptor_type::ool, 1);
        EXPECT_EQ(descriptor_type::ool_ports, 2);
        EXPECT_EQ(descriptor_type::ool_volatile, 3);
        EXPECT_EQ(descriptor_type::guarded_port, 4);

        EXPECT_EQ(copy_option::physical, 0);
        EXPECT_EQ(copy_option::virtual_copy, 1);
        EXPECT_EQ(copy_option::allocate, 2);

        EXPECT_EQ(static_cast<uint32_t>(right_kind::send), 0u);
        EXPECT_EQ(static_cast<uint32_t>(right_kind::receive), 1u);
        EXPECT_EQ(static_cast<uint32_t>(right_kind::send_once), 2u);
        EXPECT_EQ(static_cast<uint32_t>(right_kind::port_set), 3u);
        EXPECT_EQ(static_cast<uint32_t>(right_kind::dead_name), 4u);
    }

    TEST(MachTypes, MessageOptionsMatchTheHostHeaders)
    {
        EXPECT_EQ(msg_option::send_msg, 0x1u);
        EXPECT_EQ(msg_option::rcv_msg, 0x2u);
        EXPECT_EQ(msg_option::rcv_large, 0x4u);
        EXPECT_EQ(msg_option::rcv_large_identity, 0x8u);
        EXPECT_EQ(msg_option::send_timeout, 0x10u);
        EXPECT_EQ(msg_option::send_override, 0x20u);
        EXPECT_EQ(msg_option::send_interrupt, 0x40u);
        EXPECT_EQ(msg_option::send_notify, 0x80u);
        EXPECT_EQ(msg_option::rcv_timeout, 0x100u);
        EXPECT_EQ(msg_option::strict_reply, 0x200u);
        EXPECT_EQ(msg_option::rcv_interrupt, 0x400u);
        EXPECT_EQ(msg_option::rcv_voucher, 0x800u);
        EXPECT_EQ(msg_option::rcv_sync_wait, 0x4000u);
        EXPECT_EQ(msg_option::send_always, 0x10000u);
        EXPECT_EQ(msg_option::send_sync_override, 0x100000u);
        EXPECT_EQ(msg_option::vector, 0x100000000ull);
        EXPECT_EQ(msg_option::kobject_call, 0x200000000ull);
        EXPECT_EQ(msg_option::mq_call, 0x400000000ull);

        EXPECT_EQ(0x200000003ull & (msg_option::send_msg | msg_option::rcv_msg), 0x3ull);
        EXPECT_EQ(0x200000003ull & msg_option::kobject_call, msg_option::kobject_call);
        EXPECT_EQ(0x400000003ull & msg_option::kobject_call, 0ull);

        // Measured 2026-08-28: CFRunLoop's wait and libdispatch's channel sends both set the vector bit
        // alongside the mq bit, so the two say nothing about each other.
        EXPECT_EQ(0x507000806ull & msg_option::vector, msg_option::vector);
        EXPECT_EQ(0x507000806ull & msg_option::mq_call, msg_option::mq_call);
        EXPECT_EQ(0x40700420eull & msg_option::vector, 0ull);
    }

    TEST(MachTypes, SubsystemBasesAndPortIdentifiersMatchTheHostHeaders)
    {
        EXPECT_EQ(subsystem::mach_host, 200);
        EXPECT_EQ(subsystem::host_priv, 400);
        EXPECT_EQ(subsystem::clock, 1000);
        EXPECT_EQ(subsystem::exc, 2401);
        EXPECT_EQ(subsystem::mach_exc, 2405);
        EXPECT_EQ(subsystem::mach_port, 3200);
        EXPECT_EQ(subsystem::task, 3400);
        EXPECT_EQ(subsystem::thread_act, 3600);
        EXPECT_EQ(subsystem::mach_vm, 4800);
        EXPECT_EQ(subsystem::task_restartable, 8000);
        EXPECT_EQ(subsystem::reply_offset, 100);

        EXPECT_EQ(task_special_port::kernel, 1);
        EXPECT_EQ(task_special_port::host, 2);
        EXPECT_EQ(task_special_port::name, 3);
        EXPECT_EQ(task_special_port::bootstrap, 4);
        EXPECT_EQ(task_special_port::access, 9);
        EXPECT_EQ(task_special_port::debug_control, 10);
        EXPECT_EQ(task_special_port::max, 11);

        EXPECT_EQ(host_special_port::priv, 2);
        EXPECT_EQ(host_special_port::io_main, 3);
        EXPECT_EQ(host_special_port::max, 35);

        EXPECT_EQ(flavor::host_basic_info, 1u);
        EXPECT_EQ(flavor::host_basic_info_count, 12u);
        EXPECT_EQ(flavor::task_audit_token, 15u);
        EXPECT_EQ(flavor::task_audit_token_count, 8u);
        EXPECT_EQ(flavor::task_dyld_info, 17u);
        EXPECT_EQ(flavor::task_dyld_info_count, 5u);
        EXPECT_EQ(flavor::task_basic_info_64, 18u);

        EXPECT_EQ(CPU_TYPE_ARM64, 0x0100000Cu);
        EXPECT_EQ(CPU_SUBTYPE_ARM64E, 2u);
        EXPECT_EQ(SYSTEM_CLOCK, 0u);
        EXPECT_EQ(CALENDAR_CLOCK, 1u);
        EXPECT_EQ(PORT_QLIMIT_DEFAULT, 5u);
        EXPECT_EQ(PORT_QLIMIT_MAX, 1024u);
        EXPECT_EQ(MPO_CONTEXT_AS_GUARD, 0x1u);
        EXPECT_EQ(MPO_QLIMIT, 0x2u);
        EXPECT_EQ(MPO_INSERT_SEND_RIGHT, 0x10u);
        EXPECT_EQ(MPO_STRICT, 0x20u);
        EXPECT_EQ(MPO_REPLY_PORT, 0x1000u);
        EXPECT_EQ(PORT_NULL, 0u);
        EXPECT_EQ(PORT_DEAD, 0xFFFFFFFFu);
    }
}
