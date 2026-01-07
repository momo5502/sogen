#include "std_include.hpp"
#include "port.hpp"
#include "logger.hpp"
#include "windows_emulator.hpp"
#include "ports/api_port.hpp"
#include "ports/dns_resolver.hpp"

namespace
{
    struct dummy_port : port
    {
        NTSTATUS handle_request(windows_emulator& win_emu, const lpc_request_context&) override
        {
            win_emu.log.error("!!! BAD PORT\n");
            return STATUS_NOT_SUPPORTED;
        }
    };

    struct noop_port : port
    {
        NTSTATUS handle_request(windows_emulator& /*win_emu*/, const lpc_request_context& c) override
        {
            c.recv_buffer_length = 0;
            return STATUS_SUCCESS;
        }
    };
}

std::unique_ptr<port> create_port(const std::u16string_view port)
{
    if (port == u"\\Windows\\ApiPort")
    {
        return create_api_port();
    }

    if (port == u"\\RPC Control\\DNSResolver")
    {
        return create_dns_resolver();
    }

    if (port == u"\\WindowsErrorReportingServicePort")
    {
        return std::make_unique<noop_port>();
    }

    return std::make_unique<dummy_port>();
}

NTSTATUS port::handle_message(windows_emulator& win_emu, const lpc_message_context& c)
{
    const auto send_header = c.send_message.read();

    auto recv_header = send_header;
    recv_header.u2.s2.Type = LPC_REPLY;

    if (send_header.u2.s2.Type == LPC_NO_IMPERSONATE)
    {
        recv_header.u2.s2.Type |= LPC_NO_IMPERSONATE;
    }

    lpc_request_context context{};
    context.send_buffer = c.send_message.value() + sizeof(PORT_MESSAGE64);
    context.send_buffer_length = send_header.u1.s1.DataLength;
    context.recv_buffer = c.receive_message.value() + sizeof(PORT_MESSAGE64);
    context.recv_buffer_length = recv_header.u1.s1.DataLength;

    NTSTATUS status = this->handle_request(win_emu, context);

    recv_header.u1.s1.DataLength = static_cast<CSHORT>(context.recv_buffer_length);
    recv_header.u1.s1.TotalLength = static_cast<CSHORT>(sizeof(PORT_MESSAGE64) + context.recv_buffer_length);
    c.receive_message.write(recv_header);

    return status;
}

NTSTATUS rpc_port::handle_request(windows_emulator& win_emu, const lpc_request_context& c)
{
    const auto operation = win_emu.emu().read_memory<uint32_t>(c.send_buffer);

    switch (operation)
    {
    case 1: // Handshake
        return handle_handshake(win_emu, c);
    case 0: // Call
        return handle_rpc_call(win_emu, c);
    default:
        win_emu.log.print(color::gray, "Unexpected RPC operation: 0x%X\n", operation);
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS rpc_port::handle_handshake(windows_emulator& win_emu, const lpc_request_context& c)
{
    win_emu.emu().write_memory<uint32_t>(c.recv_buffer + 8, 0);

    if (win_emu.emu().read_memory<uint32_t>(c.send_buffer + 32) == 3)
    {
        win_emu.emu().write_memory<uint32_t>(c.recv_buffer + 32, 2);
    }

    return STATUS_SUCCESS;
}

NTSTATUS rpc_port::handle_rpc_call(windows_emulator& win_emu, const lpc_request_context& c)
{
    const auto procedure_id = win_emu.emu().read_memory<uint8_t>(c.send_buffer + 12);

    std::array<uint8_t, 24> header = {0x03,         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      procedure_id, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    win_emu.emu().write_memory(c.recv_buffer, header);

    lpc_request_context rpc_context{};
    rpc_context.send_buffer = c.send_buffer + 0x40;
    rpc_context.send_buffer_length = c.send_buffer_length - 0x40;
    rpc_context.recv_buffer = c.recv_buffer + sizeof(header);
    rpc_context.recv_buffer_length = c.recv_buffer_length;

    NTSTATUS status = this->handle_rpc(win_emu, procedure_id, rpc_context);
    c.recv_buffer_length = sizeof(header) + rpc_context.recv_buffer_length;

    return status;
}
