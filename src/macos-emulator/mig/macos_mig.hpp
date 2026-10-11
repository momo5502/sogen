#pragma once

#include "../macos_mach_ports.hpp"
#include "../macos_syscall_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace sogen
{
    struct mach_msg_header
    {
        uint32_t bits;
        uint32_t size;
        uint32_t remote_port;
        uint32_t local_port;
        uint32_t voucher_port;
        int32_t id;
    };

    static_assert(sizeof(mach_msg_header) == 24);

    // MIG requests and replies carry an NDR record right after the header.
    constexpr size_t MIG_BODY_OFFSET = sizeof(mach_msg_header) + 8;

    // An empty reply means the routine could not answer; the caller logs it and stops emulation.
    using mig_routine = std::vector<std::byte> (*)(const macos_syscall_context& c, std::span<const std::byte> request);

    mig_routine find_mig_routine(mach_port_object object, int32_t id);

    std::vector<std::byte> mig_host_info(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_host_get_clock_service(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_semaphore_create(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_task_restartable_ranges_register(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_task_get_special_port(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_task_set_special_port(const macos_syscall_context& c, std::span<const std::byte> request);
    std::vector<std::byte> mig_task_info(const macos_syscall_context& c, std::span<const std::byte> request);

    template <typename T>
    T read_mig_field(const std::span<const std::byte> message, const size_t offset)
    {
        if (offset > message.size() || message.size() - offset < sizeof(T))
        {
            throw std::runtime_error("Truncated MIG message");
        }

        T value{};
        std::memcpy(&value, message.data() + offset, sizeof(T));
        return value;
    }

    constexpr uint32_t MACH_MSGH_BITS_LOCAL_SEND_ONCE = 18 << 8;
    constexpr int32_t MIG_REPLY_ID_OFFSET = 100;

    // A complex reply carrying a single send right, laid out as MIG expects: header with the complex bit, one
    // descriptor, then the port descriptor itself (no NDR record).
    inline std::vector<std::byte> make_mig_port_reply(const std::span<const std::byte> request, const uint32_t port_name)
    {
        constexpr uint32_t mach_msgh_bits_complex = 0x80000000;
        constexpr uint8_t mach_msg_type_port_send = 17;
        constexpr uint8_t mach_msg_port_descriptor = 0;

        struct port_reply
        {
            mach_msg_header header;
            uint32_t descriptor_count;
            uint32_t name;
            uint32_t pad1;
            uint16_t pad2;
            uint8_t disposition;
            uint8_t type;
        };

        static_assert(sizeof(port_reply) == 40);

        const auto request_header = read_mig_field<mach_msg_header>(request, 0);
        const port_reply reply{
            .header =
                {
                    .bits = MACH_MSGH_BITS_LOCAL_SEND_ONCE | mach_msgh_bits_complex,
                    .size = sizeof(port_reply),
                    .remote_port = 0,
                    .local_port = request_header.local_port,
                    .voucher_port = 0,
                    .id = request_header.id + MIG_REPLY_ID_OFFSET,
                },
            .descriptor_count = 1,
            .name = port_name,
            .pad1 = 0,
            .pad2 = 0,
            .disposition = mach_msg_type_port_send,
            .type = mach_msg_port_descriptor,
        };

        std::vector<std::byte> data(sizeof(reply));
        std::memcpy(data.data(), &reply, sizeof(reply));
        return data;
    }

    // A reply as the kernel delivers it: received on the request's reply port through a send-once right,
    // with the id MIG expects (request id + 100).
    inline std::vector<std::byte> make_mig_reply(const std::span<const std::byte> request, const std::span<const std::byte> body)
    {
        constexpr std::array<uint8_t, 8> ndr_record{0, 0, 0, 0, 1, 0, 0, 0};

        const auto request_header = read_mig_field<mach_msg_header>(request, 0);

        std::vector<std::byte> reply(MIG_BODY_OFFSET + body.size());
        const mach_msg_header reply_header{
            .bits = MACH_MSGH_BITS_LOCAL_SEND_ONCE,
            .size = static_cast<uint32_t>(reply.size()),
            .remote_port = 0,
            .local_port = request_header.local_port,
            .voucher_port = 0,
            .id = request_header.id + MIG_REPLY_ID_OFFSET,
        };

        std::memcpy(reply.data(), &reply_header, sizeof(reply_header));
        std::memcpy(reply.data() + sizeof(reply_header), ndr_record.data(), ndr_record.size());
        std::memcpy(reply.data() + MIG_BODY_OFFSET, body.data(), body.size());
        return reply;
    }

    inline std::vector<std::byte> make_mig_return_code_reply(const std::span<const std::byte> request, const uint64_t return_code)
    {
        const auto code = static_cast<int32_t>(return_code);
        std::array<std::byte, sizeof(code)> body{};
        std::memcpy(body.data(), &code, sizeof(code));
        return make_mig_reply(request, body);
    }
}
