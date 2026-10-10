#pragma once

#include "../macos_mach_ports.hpp"
#include "../macos_syscall_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

    using mig_routine = std::vector<std::byte> (*)(const macos_syscall_context& c, std::span<const std::byte> request);

    mig_routine find_mig_routine(mach_port_object object, int32_t id);

    std::vector<std::byte> mig_host_info(const macos_syscall_context& c, std::span<const std::byte> request);

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

    // A reply as the kernel delivers it: received on the request's reply port through a send-once right,
    // with the id MIG expects (request id + 100).
    inline std::vector<std::byte> make_mig_reply(const std::span<const std::byte> request, const std::span<const std::byte> body)
    {
        constexpr uint32_t local_send_once_bits = 18 << 8;
        constexpr std::array<uint8_t, 8> ndr_record{0, 0, 0, 0, 1, 0, 0, 0};
        constexpr int32_t reply_id_offset = 100;

        const auto request_header = read_mig_field<mach_msg_header>(request, 0);

        std::vector<std::byte> reply(MIG_BODY_OFFSET + body.size());
        const mach_msg_header reply_header{
            .bits = local_send_once_bits,
            .size = static_cast<uint32_t>(reply.size()),
            .remote_port = 0,
            .local_port = request_header.local_port,
            .voucher_port = 0,
            .id = request_header.id + reply_id_offset,
        };

        std::memcpy(reply.data(), &reply_header, sizeof(reply_header));
        std::memcpy(reply.data() + sizeof(reply_header), ndr_record.data(), ndr_record.size());
        std::memcpy(reply.data() + MIG_BODY_OFFSET, body.data(), body.size());
        return reply;
    }
}
