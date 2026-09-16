#pragma once

#include "mach_types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace sogen
{
    class macos_emulator;
    struct macos_syscall_context;
}

namespace sogen::mach
{
    // `buffer`, `send_size` and `rcv_size` are always the message and its byte counts, whichever form
    // the trap used: MACH64_MSG_VECTOR is resolved in decode_msg2_call, so nothing downstream sees the
    // vector at all. `rcv_buffer` differs from `buffer` only when the vector named a separate receive
    // address (mach_msg_vector_t::msgv_rcv_addr).
    struct msg_call
    {
        uint64_t buffer{};
        uint64_t options{};
        msg_header header{};
        uint32_t send_size{};
        uint32_t descriptor_count{};
        port_name_t rcv_name{};
        uint32_t rcv_size{};
        uint32_t priority{};
        uint64_t timeout{};
        uint64_t rcv_buffer{};
        uint64_t aux_buffer{};
        uint32_t aux_send_size{};
        uint32_t aux_rcv_size{};

        // A vector the trap layer could not resolve. perform_msg returns it before doing anything, so
        // the trap answers what the kernel would answer instead of acting on a half-decoded call.
        mach_msg_return_t decode_error{};
    };

    struct msg_reply
    {
        std::vector<uint8_t> bytes{};
        bool valid{};
    };

    msg_call decode_msg2_call(const macos_syscall_context& c);
    std::vector<uint8_t> read_message_body(macos_emulator& emu, const msg_call& call);
    mach_msg_return_t perform_msg(macos_emulator& emu, const msg_call& call);
    mach_msg_return_t deliver_reply(macos_emulator& emu, const msg_call& call, std::span<const uint8_t> reply);
    msg_reply make_mig_error_reply(const msg_call& call, kern_return_t code);
    uint32_t reply_bits_for(uint32_t request_bits, bool complex);

    // make- and copy- are instructions to the kernel about a right to create; a message being
    // *received* can only carry the right itself, so it says move-. Applied to both the header and
    // every descriptor, because a guest's message-teardown path reads them the same way.
    uint8_t received_disposition(uint8_t sent);

    // Wakes every thread parked on `name`, and on each port set `name` is a member of. Returns how
    // many threads were unparked.
    size_t wake_port_receivers(macos_emulator& emu, port_name_t name);

    // Announces a message that has just been queued on `name`: unparks its receivers, fires the kqueue
    // knotes watching it, and reports a message nothing at all is waiting for.
    void announce_queued_message(macos_emulator& emu, port_name_t name, int32_t routine);

    // Queues the header-only message xnu's mk_timer_expire sends, and wakes whoever is waiting for it.
    void deliver_timer_expiration(macos_emulator& emu, port_name_t name);
}
