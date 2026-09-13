#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sogen::mach
{
    using kern_return_t = int32_t;
    using mach_msg_return_t = uint32_t;
    using port_name_t = uint32_t;

    constexpr port_name_t PORT_NULL = 0;
    constexpr port_name_t PORT_DEAD = 0xFFFFFFFFu;

    constexpr port_name_t make_port_name(const uint32_t index, const uint8_t generation)
    {
        return (index << 8) | generation;
    }

    constexpr uint32_t port_index(const port_name_t name)
    {
        return name >> 8;
    }

    constexpr uint8_t port_generation(const port_name_t name)
    {
        return static_cast<uint8_t>(name & 0xFFu);
    }

    enum class right_kind : uint32_t
    {
        send = 0,
        receive = 1,
        send_once = 2,
        port_set = 3,
        dead_name = 4,
    };

    namespace kr
    {
        constexpr kern_return_t success = 0;
        constexpr kern_return_t invalid_address = 1;
        constexpr kern_return_t protection_failure = 2;
        constexpr kern_return_t no_space = 3;
        constexpr kern_return_t invalid_argument = 4;
        constexpr kern_return_t failure = 5;
        constexpr kern_return_t resource_shortage = 6;
        constexpr kern_return_t not_receiver = 7;
        constexpr kern_return_t no_access = 8;
        constexpr kern_return_t memory_error = 10;
        constexpr kern_return_t already_in_set = 11;
        constexpr kern_return_t not_in_set = 12;
        constexpr kern_return_t name_exists = 13;
        constexpr kern_return_t invalid_name = 15;
        constexpr kern_return_t invalid_task = 16;
        constexpr kern_return_t invalid_right = 17;
        constexpr kern_return_t invalid_value = 18;
        constexpr kern_return_t urefs_overflow = 19;
        constexpr kern_return_t invalid_capability = 20;
        constexpr kern_return_t right_exists = 21;
        constexpr kern_return_t terminated = 37;
        constexpr kern_return_t not_supported = 46;
        constexpr kern_return_t operation_timed_out = 49;
    }

    namespace msgr
    {
        constexpr mach_msg_return_t success = 0;
        constexpr mach_msg_return_t send_invalid_data = 0x10000002u;
        constexpr mach_msg_return_t send_invalid_dest = 0x10000003u;
        constexpr mach_msg_return_t send_timed_out = 0x10000004u;
        constexpr mach_msg_return_t send_msg_too_small = 0x10000008u;
        constexpr mach_msg_return_t send_invalid_reply = 0x10000009u;
        constexpr mach_msg_return_t send_invalid_type = 0x1000000fu;
        constexpr mach_msg_return_t send_invalid_header = 0x10000010u;
        constexpr mach_msg_return_t rcv_invalid_name = 0x10004002u;
        constexpr mach_msg_return_t rcv_timed_out = 0x10004003u;

        // MACH_RCV_INTERRUPTED. Never seen by the guest: the thread that would receive it has been parked
        // with its pc on its own svc, so the value is only a signal to the caller of the message layer
        // that the register file now belongs to a different thread and must not be written.
        constexpr mach_msg_return_t rcv_interrupted = 0x10004005u;
        constexpr mach_msg_return_t rcv_too_large = 0x10004004u;
        constexpr mach_msg_return_t rcv_invalid_data = 0x10004008u;
        constexpr mach_msg_return_t rcv_port_died = 0x10004009u;
        constexpr mach_msg_return_t rcv_header_error = 0x1000400bu;
        constexpr mach_msg_return_t rcv_invalid_arguments = 0x10004013u;
    }

    namespace mig_error
    {
        constexpr kern_return_t type_error = -300;
        constexpr kern_return_t reply_mismatch = -301;
        constexpr kern_return_t remote_error = -302;
        constexpr kern_return_t bad_id = -303;
        constexpr kern_return_t bad_arguments = -304;
        constexpr kern_return_t array_too_large = -307;
        constexpr kern_return_t server_died = -308;
    }

    namespace disposition
    {
        constexpr uint8_t move_receive = 16;
        constexpr uint8_t move_send = 17;
        constexpr uint8_t move_send_once = 18;
        constexpr uint8_t copy_send = 19;
        constexpr uint8_t make_send = 20;
        constexpr uint8_t make_send_once = 21;
        constexpr uint8_t copy_receive = 22;
        constexpr uint8_t dispose_receive = 24;
        constexpr uint8_t dispose_send = 25;
        constexpr uint8_t dispose_send_once = 26;
    }

    namespace descriptor_type
    {
        constexpr uint8_t port = 0;
        constexpr uint8_t ool = 1;
        constexpr uint8_t ool_ports = 2;
        constexpr uint8_t ool_volatile = 3;
        constexpr uint8_t guarded_port = 4;
    }

    namespace copy_option
    {
        constexpr uint8_t physical = 0;
        constexpr uint8_t virtual_copy = 1;
        constexpr uint8_t allocate = 2;
    }

    namespace msg_option
    {
        constexpr uint64_t send_msg = 0x1;
        constexpr uint64_t rcv_msg = 0x2;
        constexpr uint64_t rcv_large = 0x4;
        constexpr uint64_t rcv_large_identity = 0x8;
        constexpr uint64_t send_timeout = 0x10;
        constexpr uint64_t send_override = 0x20;
        constexpr uint64_t send_interrupt = 0x40;
        constexpr uint64_t send_notify = 0x80;
        constexpr uint64_t rcv_timeout = 0x100;
        constexpr uint64_t strict_reply = 0x200;
        constexpr uint64_t rcv_interrupt = 0x400;
        constexpr uint64_t rcv_voucher = 0x800;
        constexpr uint64_t rcv_sync_wait = 0x4000;
        constexpr uint64_t send_always = 0x10000;
        constexpr uint64_t send_sync_override = 0x100000;
        constexpr uint64_t vector = 0x100000000ull;
        constexpr uint64_t kobject_call = 0x200000000ull;
        constexpr uint64_t mq_call = 0x400000000ull;
    }

    namespace subsystem
    {
        constexpr int32_t mach_host = 200;
        constexpr int32_t host_priv = 400;
        constexpr int32_t clock = 1000;
        constexpr int32_t exc = 2401;
        constexpr int32_t mach_exc = 2405;
        constexpr int32_t mach_port = 3200;
        constexpr int32_t task = 3400;
        constexpr int32_t thread_act = 3600;
        constexpr int32_t mach_vm = 4800;
        constexpr int32_t task_restartable = 8000;
        constexpr int32_t reply_offset = 100;
    }

    namespace flavor
    {
        constexpr uint32_t host_basic_info = 1;
        constexpr uint32_t host_basic_info_count = 12;
        constexpr uint32_t host_priority_info = 5;
        constexpr uint32_t host_priority_info_count = 8;
        constexpr uint32_t task_audit_token = 15;
        constexpr uint32_t task_audit_token_count = 8;
        constexpr uint32_t task_dyld_info = 17;
        constexpr uint32_t task_dyld_info_count = 5;
        constexpr uint32_t task_dyld_all_image_info_64 = 1;
        constexpr uint32_t task_basic_info_64 = 18;
    }

    namespace task_special_port
    {
        constexpr int32_t kernel = 1;
        constexpr int32_t host = 2;
        constexpr int32_t name = 3;
        constexpr int32_t bootstrap = 4;
        constexpr int32_t access = 9;
        constexpr int32_t debug_control = 10;
        constexpr int32_t max = 11;
    }

    namespace host_special_port
    {
        constexpr int32_t priv = 2;
        constexpr int32_t io_main = 3;
        constexpr int32_t max = 35;
    }

    constexpr uint32_t CPU_TYPE_ARM64 = 0x0100000Cu;
    constexpr uint32_t CPU_SUBTYPE_ARM64E = 2;
    constexpr uint32_t SYSTEM_CLOCK = 0;
    constexpr uint32_t CALENDAR_CLOCK = 1;
    constexpr uint32_t PORT_QLIMIT_DEFAULT = 5;
    constexpr uint32_t PORT_QLIMIT_MAX = 1024;
    constexpr uint32_t MPO_CONTEXT_AS_GUARD = 0x1;
    constexpr uint32_t MPO_QLIMIT = 0x2;
    constexpr uint32_t MPO_INSERT_SEND_RIGHT = 0x10;
    constexpr uint32_t MPO_STRICT = 0x20;
    constexpr uint32_t MPO_REPLY_PORT = 0x1000;

    constexpr size_t MSG_HEADER_SIZE = 24;
    constexpr size_t MSG_BODY_SIZE = 4;
    constexpr size_t PORT_DESCRIPTOR_SIZE = 12;
    constexpr size_t OOL_DESCRIPTOR_SIZE = 16;
    constexpr size_t NDR_RECORD_SIZE = 8;
    constexpr size_t MIG_REPLY_ERROR_SIZE = 36;
    constexpr size_t TRAILER_SIZE = 8;
    constexpr size_t MSG_SIZE_MAX = 0x40000;

    // MACH64_MSG_VECTOR: the trap's buffer is an array of mach_msg_vector_t, and the two size halves of
    // the trap carry element counts rather than byte counts. xnu takes exactly two elements -- the
    // message and the auxiliary data -- and answers a third with MACH_SEND_INVALID_DATA on the send side
    // and MACH_RCV_INVALID_ARGUMENTS on the receive side (measured 2026-08-28 on the host, by forcing
    // the counts in the register file of a live AppKit process).
    constexpr size_t MSG_VECTOR_ELEMENT_SIZE = 24;
    constexpr uint32_t MSG_VECTOR_MAX_COUNT = 2;
    constexpr uint32_t MSG_VECTOR_INDEX_MESSAGE = 0;
    constexpr uint32_t MSG_VECTOR_INDEX_AUX = 1;

    // mach_msg_aux_header_t. A receive whose message carries no aux gets this header zeroed, which is
    // how the guest tells "no auxiliary data" from whatever its buffer held before.
    constexpr size_t MSG_AUX_HEADER_SIZE = 8;

    constexpr uint32_t BITS_COMPLEX = 0x80000000u;
    constexpr uint32_t BITS_REMOTE_MASK = 0x1Fu;
    constexpr uint32_t BITS_LOCAL_MASK = 0x1F00u;
    constexpr uint32_t BITS_VOUCHER_MASK = 0x1F0000u;

    constexpr std::array<uint8_t, NDR_RECORD_SIZE> NDR_RECORD{0, 0, 0, 0, 1, 0, 0, 0};

    constexpr uint32_t make_bits(const uint8_t remote, const uint8_t local)
    {
        return (static_cast<uint32_t>(remote) & BITS_REMOTE_MASK) | ((static_cast<uint32_t>(local) << 8) & BITS_LOCAL_MASK);
    }

    constexpr uint8_t remote_disposition(const uint32_t bits)
    {
        return static_cast<uint8_t>(bits & BITS_REMOTE_MASK);
    }

    constexpr uint8_t local_disposition(const uint32_t bits)
    {
        return static_cast<uint8_t>((bits & BITS_LOCAL_MASK) >> 8);
    }

    constexpr uint8_t voucher_disposition(const uint32_t bits)
    {
        return static_cast<uint8_t>((bits & BITS_VOUCHER_MASK) >> 16);
    }

    namespace detail
    {
        constexpr bool fits(const size_t size, const size_t offset, const size_t length)
        {
            return offset <= size && length <= size - offset;
        }
    }

    // The spans these accessors serve are sized by the guest (a `send_size` from `mach_msg2_trap`), so an
    // out-of-range access is reachable input rather than a programming error: it drops the write and reads
    // as zero instead of stepping off the end of the buffer.
    constexpr void write_u32(const std::span<uint8_t> out, const size_t offset, const uint32_t value)
    {
        if (!detail::fits(out.size(), offset, 4))
        {
            return;
        }

        out[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
        out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    }

    constexpr void write_u64(const std::span<uint8_t> out, const size_t offset, const uint64_t value)
    {
        if (!detail::fits(out.size(), offset, 8))
        {
            return;
        }

        write_u32(out, offset, static_cast<uint32_t>(value & 0xFFFFFFFFull));
        write_u32(out, offset + 4, static_cast<uint32_t>(value >> 32));
    }

    constexpr uint32_t read_u32(const std::span<const uint8_t> in, const size_t offset)
    {
        if (!detail::fits(in.size(), offset, 4))
        {
            return 0;
        }

        return static_cast<uint32_t>(in[offset + 0]) | (static_cast<uint32_t>(in[offset + 1]) << 8) |
               (static_cast<uint32_t>(in[offset + 2]) << 16) | (static_cast<uint32_t>(in[offset + 3]) << 24);
    }

    constexpr uint64_t read_u64(const std::span<const uint8_t> in, const size_t offset)
    {
        if (!detail::fits(in.size(), offset, 8))
        {
            return 0;
        }

        return static_cast<uint64_t>(read_u32(in, offset)) | (static_cast<uint64_t>(read_u32(in, offset + 4)) << 32);
    }

    struct msg_header
    {
        uint32_t bits{};
        uint32_t size{};
        port_name_t remote_port{};
        port_name_t local_port{};
        port_name_t voucher_port{};
        int32_t id{};
    };

    constexpr void write_msg_header(const std::span<uint8_t> out, const msg_header& header)
    {
        write_u32(out, 0, header.bits);
        write_u32(out, 4, header.size);
        write_u32(out, 8, header.remote_port);
        write_u32(out, 12, header.local_port);
        write_u32(out, 16, header.voucher_port);
        write_u32(out, 20, static_cast<uint32_t>(header.id));
    }

    constexpr msg_header read_msg_header(const std::span<const uint8_t> in)
    {
        return {
            .bits = read_u32(in, 0),
            .size = read_u32(in, 4),
            .remote_port = read_u32(in, 8),
            .local_port = read_u32(in, 12),
            .voucher_port = read_u32(in, 16),
            .id = static_cast<int32_t>(read_u32(in, 20)),
        };
    }

    struct msg_vector_element
    {
        uint64_t data{};
        uint64_t rcv_address{};
        uint32_t send_size{};
        uint32_t rcv_size{};
    };

    constexpr void write_msg_vector_element(const std::span<uint8_t> out, const msg_vector_element& element)
    {
        write_u64(out, 0, element.data);
        write_u64(out, 8, element.rcv_address);
        write_u32(out, 16, element.send_size);
        write_u32(out, 20, element.rcv_size);
    }

    constexpr msg_vector_element read_msg_vector_element(const std::span<const uint8_t> in)
    {
        return {
            .data = read_u64(in, 0),
            .rcv_address = read_u64(in, 8),
            .send_size = read_u32(in, 16),
            .rcv_size = read_u32(in, 20),
        };
    }

    struct port_descriptor
    {
        port_name_t name{};
        uint8_t disposition{};
        uint8_t type{};
    };

    constexpr void write_port_descriptor(const std::span<uint8_t> out, const port_descriptor& descriptor)
    {
        write_u32(out, 0, descriptor.name);
        write_u32(out, 4, 0);
        write_u32(out, 8, (static_cast<uint32_t>(descriptor.disposition) << 16) | (static_cast<uint32_t>(descriptor.type) << 24));
    }

    constexpr port_descriptor read_port_descriptor(const std::span<const uint8_t> in)
    {
        const auto tail = read_u32(in, 8);
        return {
            .name = read_u32(in, 0),
            .disposition = static_cast<uint8_t>((tail >> 16) & 0xFFu),
            .type = static_cast<uint8_t>((tail >> 24) & 0xFFu),
        };
    }

    struct ool_descriptor
    {
        uint64_t address{};
        uint32_t size{};
        uint8_t deallocate{};
        uint8_t copy{};
        uint8_t type{};
    };

    constexpr void write_ool_descriptor(const std::span<uint8_t> out, const ool_descriptor& descriptor)
    {
        write_u64(out, 0, descriptor.address);
        write_u32(out, 8,
                  static_cast<uint32_t>(descriptor.deallocate) | (static_cast<uint32_t>(descriptor.copy) << 8) |
                      (static_cast<uint32_t>(descriptor.type) << 24));
        write_u32(out, 12, descriptor.size);
    }

    constexpr ool_descriptor read_ool_descriptor(const std::span<const uint8_t> in)
    {
        const auto flags = read_u32(in, 8);
        return {
            .address = read_u64(in, 0),
            .size = read_u32(in, 12),
            .deallocate = static_cast<uint8_t>(flags & 0xFFu),
            .copy = static_cast<uint8_t>((flags >> 8) & 0xFFu),
            .type = static_cast<uint8_t>((flags >> 24) & 0xFFu),
        };
    }

    constexpr size_t descriptor_size(const uint8_t type)
    {
        return type == descriptor_type::port ? PORT_DESCRIPTOR_SIZE : OOL_DESCRIPTOR_SIZE;
    }
}
