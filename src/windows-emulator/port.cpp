#include "std_include.hpp"
#include "port.hpp"
#include "logger.hpp"
#include "windows_emulator.hpp"
#include "ports/api_port.hpp"
#include "ports/core_messaging_registrar.hpp"
#include "ports/dns_resolver.hpp"
#include "ports/lsa_policy_lookup.hpp"
#include "binary_writer.hpp"

namespace sogen
{

    namespace
    {
        struct dummy_port : port
        {
            lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context&) override
            {
                win_emu.log.error("!!! BAD PORT\n");
                return STATUS_NOT_SUPPORTED;
            }
        };

        struct noop_port : port
        {
            lpc_request_result handle_request(windows_emulator& /*win_emu*/, const lpc_request_context&) override
            {
                return STATUS_SUCCESS;
            }
        };

        struct noop_rpc_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& /*win_emu*/, const uint32_t /*procedure_id*/, const lpc_request_context&,
                                utils::aligned_binary_writer& /*writer*/) override
            {
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

        if (port == u"\\RPC Control\\LSARPC_ENDPOINT" || port == u"\\RPC Control\\lsapolicylookup")
        {
            return create_lsa_policy_lookup_port();
        }

        if (port == u"\\WindowsErrorReportingServicePort")
        {
            return std::make_unique<noop_port>();
        }

        if (port == u"\\BaseNamedObjects\\CoreMessagingRegistrar")
        {
            return create_core_messaging_registrar_port();
        }

        if (port == u"\\RPC Control\\ntsvcs")
        {
            // Service-control RPC is probed during network stack initialization.
            // A zero-payload RPC success is enough for the current callers to continue
            // instead of turning the probe into a hard network failure.
            return std::make_unique<noop_rpc_port>();
        }

        return std::make_unique<dummy_port>();
    }

    lpc_message_result port_container::handle_message(windows_emulator& win_emu, const lpc_message_context& c)
    {
        this->assert_validity();

        if (!c.receive_message)
        {
            return {.status = STATUS_INVALID_PARAMETER};
        }

        if (!c.send_message)
        {
            if (reply_queue_.empty())
            {
                return {.status = STATUS_UNSUCCESSFUL};
            }

            auto& pending_reply = reply_queue_.front();
            const auto required_length = pending_reply.total_length();
            if (c.receive_buffer_length < required_length)
            {
                return {.status = STATUS_BUFFER_TOO_SMALL, .message = pending_reply.message};
            }

            lpc_message_result result = std::move(pending_reply);
            result.status = STATUS_SUCCESS;
            reply_queue_.erase(reply_queue_.begin());
            return result;
        }

        auto result = this->port_->handle_message(win_emu, c);

        if (NT_SUCCESS(result.status) && c.receive_buffer_length < result.total_length())
        {
            const auto message = result.message;
            reply_queue_.push_back(std::move(result));
            return {.status = STATUS_BUFFER_TOO_SMALL, .message = message};
        }

        return result;
    }

    lpc_message_result port::handle_message(windows_emulator& win_emu, const lpc_message_context& c)
    {
        if (!c.send_message)
        {
            return {.status = STATUS_INVALID_PARAMETER};
        }

        const auto send_header = lpc_port_message::read(c.send_message);

        const auto data_length = send_header.data_length();
        const auto total_length = send_header.total_length();

        if (total_length < data_length)
        {
            return {.status = STATUS_INVALID_PARAMETER};
        }

        const auto header_size = send_header.header_size();

        if (header_size != send_header.wire_size())
        {
            return {.status = STATUS_INVALID_PARAMETER};
        }

        auto recv_header = send_header;
        recv_header.native.u2.s2.Type = LPC_REPLY;

        if (send_header.native.u2.s2.Type == LPC_NO_IMPERSONATE)
        {
            recv_header.native.u2.s2.Type |= LPC_NO_IMPERSONATE;
        }

        lpc_request_context context{};
        context.send_buffer = c.send_message.value() + header_size;
        context.send_buffer_length = data_length;
        context.recv_buffer = c.receive_message ? c.receive_message.value() + header_size : 0;
        context.recv_buffer_length =
            c.receive_buffer_length >= header_size ? static_cast<ULONG>(c.receive_buffer_length - header_size) : data_length;

        auto request_result = this->handle_request(win_emu, context);
        const auto payload_size = request_result.payload ? static_cast<ULONG>(request_result.payload->size()) : context.recv_buffer_length;

        if (header_size + payload_size > static_cast<ULONG>(std::numeric_limits<CSHORT>::max()))
        {
            throw std::runtime_error("Response payload too big");
        }

        recv_header.native.u1.s1.DataLength = static_cast<CSHORT>(payload_size);
        recv_header.native.u1.s1.TotalLength = static_cast<CSHORT>(header_size + payload_size);

        lpc_message_result result{.status = request_result.status, .message = recv_header};
        if (request_result.payload)
        {
            result.payload = std::move(*request_result.payload);
        }

        return result;
    }

    lpc_request_result rpc_port::handle_request(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_op_size = sizeof(uint32_t);
        if (c.send_buffer_length < rpc_op_size)
        {
            return STATUS_INVALID_PARAMETER;
        }

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

    lpc_request_result rpc_port::handle_handshake(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_handshake_state_offset = 32;
        constexpr ULONG required_handshake_read_bytes = rpc_handshake_state_offset + sizeof(uint32_t);

        if (c.send_buffer_length < required_handshake_read_bytes || c.recv_buffer_length < c.send_buffer_length)
        {
            return STATUS_INVALID_PARAMETER;
        }

        std::vector<uint8_t> payload(c.send_buffer_length, 0);
        win_emu.emu().read_memory(c.recv_buffer, payload.data(), payload.size());

        utils::aligned_binary_writer writer(payload);
        writer.write_at<uint32_t>(8, 0);

        if (win_emu.emu().read_memory<uint32_t>(c.send_buffer + rpc_handshake_state_offset) == 3)
        {
            writer.write_at<uint32_t>(rpc_handshake_state_offset, 2);
        }

        return {STATUS_SUCCESS, std::move(payload)};
    }

    lpc_request_result rpc_port::handle_rpc_call(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_call_send_header_size = 0x40;
        constexpr ULONG rpc_call_id_offset = 12;
        constexpr ULONG rpc_call_opnum_offset = 20;

        if (c.send_buffer_length < rpc_call_send_header_size)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto call_id = win_emu.emu().read_memory<uint32_t>(c.send_buffer + rpc_call_id_offset);
        const auto procedure_id = win_emu.emu().read_memory<uint32_t>(c.send_buffer + rpc_call_opnum_offset);

        std::array<uint8_t, 24> header = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(header.data() + 12, &call_id, sizeof(call_id));

        std::vector<uint8_t> payload;
        utils::aligned_binary_writer writer(payload);
        writer.write(header.data(), header.size());

        lpc_request_context rpc_context{};
        rpc_context.send_buffer = c.send_buffer + rpc_call_send_header_size;
        rpc_context.send_buffer_length = c.send_buffer_length - rpc_call_send_header_size;
        rpc_context.recv_buffer = c.recv_buffer + sizeof(header);
        if (c.recv_buffer_length >= header.size())
        {
            rpc_context.recv_buffer_length = c.recv_buffer_length - static_cast<DWORD>(header.size());
        }

        const auto status = this->handle_rpc(win_emu, procedure_id, rpc_context, writer);

        return {status, std::move(payload)};
    }

} // namespace sogen
