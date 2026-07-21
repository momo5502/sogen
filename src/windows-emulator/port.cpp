#include "std_include.hpp"
#include "port.hpp"
#include "logger.hpp"
#include "windows_emulator.hpp"
#include "ports/api_port.hpp"
#include "ports/audio_service.hpp"
#include "ports/core_messaging_registrar.hpp"
#include "ports/dns_resolver.hpp"
#include "ports/lsa_policy_lookup.hpp"
#include "ports/service_control.hpp"
#include "binary_writer.hpp"

#include <platform/unicode.hpp>

namespace sogen
{

    namespace
    {
        struct dummy_port : port
        {
            explicit dummy_port(std::u16string name)
                : name_(std::move(name))
            {
            }

            lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context&) override
            {
                win_emu.log.error("!!! BAD PORT: %s\n", u16_to_u8(name_).c_str());
                return STATUS_NOT_SUPPORTED;
            }

            std::u16string name_;
        };

        struct noop_port : port
        {
            lpc_request_result handle_request(windows_emulator& /*win_emu*/, const lpc_request_context&) override
            {
                return STATUS_SUCCESS;
            }
        };

        // The per-stream WASAPI CrossProcessEndpoint control channel. Event-driven (AUDCLNT_STREAMFLAGS_
        // EVENTCALLBACK) DirectSound clients connect a dedicated ALPC port and send small fixed command
        // messages here (SetEventHandle, opcode in the first data dword, the event handle as a client->server
        // ALPC handle attribute), then read an NTSTATUS the server writes at data offset 4. The engine side is
        // this emulator (the render_stream drains the shared buffer directly, so we do not need the event), so
        // acknowledge every command with STATUS_SUCCESS. Without this the send lands on a dummy port
        // (STATUS_NOT_SUPPORTED -> AUDCLNT NOT_SUPPORTED) and Initialize fails.
        struct endpoint_control_port : port
        {
            lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context& c) override
            {
                // SetEventHandle delivers the client's render event as a client->server ALPC handle attribute.
                // Remember it so the audio render thread can signal it at the device rate (EVENTCALLBACK); without
                // that wake the client writes one pre-roll buffer and then blocks forever waiting on the event.
                if (c.send_handle)
                {
                    win_emu.process.audio_render_event = win_emu.process.events.get(c.send_handle);
                }

                std::vector<uint8_t> payload(c.send_buffer_length, 0);
                if (c.send_buffer && c.send_buffer_length)
                {
                    win_emu.emu().read_memory(c.send_buffer, payload.data(), payload.size());
                }

                constexpr size_t status_offset = 4; // command status the client reads back
                if (payload.size() >= status_offset + sizeof(uint32_t))
                {
                    std::memset(payload.data() + status_offset, 0, sizeof(uint32_t));
                }

                return {STATUS_SUCCESS, std::move(payload)};
            }
        };

        // Minimal RPC stub: completes the LRPC bind and answers every call with an S_OK return and no [out]
        // data. Enough for fire-and-forget power/notification registrations (e.g. \RPC Control\umpo) that the
        // audio stack performs during stream setup and only checks for success.
        struct stub_rpc_port : rpc_port
        {
            NTSTATUS handle_rpc(windows_emulator& /*win_emu*/, uint32_t /*procedure_id*/, const lpc_request_context& /*c*/,
                                utils::aligned_binary_writer& writer, std::vector<alpc_reply_handle>& /*reply_handles*/) override
            {
                writer.write<uint32_t>(0); // return HRESULT S_OK
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

        if (port == u"\\RPC Control\\Audiosrv" || port == u"\\RPC Control\\AudioClientRpc" || port == u"\\RPC Control\\AudioSrvServiceRpc")
        {
            return create_audio_service_port();
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
            // Hosts the svcctl (Service Control Manager) and PnP RPC interfaces. mmdevapi opens the AudioSrv
            // service through svcctl while creating a render audio client, so the open/close calls must return
            // a real context handle; other interfaces on this port fall back to a zero-payload success.
            return create_service_control_port();
        }

        if (port == u"\\RPC Control\\umpo")
        {
            // User-Mode Power Orchestrator. The audio stack registers a power request here while starting a
            // stream; without a responder the render worker retries and then stalls.
            return std::make_unique<stub_rpc_port>();
        }

        if (port.empty())
        {
            // Unnamed ALPC ports in the audio stack are the per-stream WASAPI endpoint control channel.
            return std::make_unique<endpoint_control_port>();
        }

        return std::make_unique<dummy_port>(std::u16string(port));
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
        context.send_handle = c.send_handle;

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
        result.handles = std::move(request_result.handles);

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

        // The bind carries the target interface's RPC_SYNTAX_IDENTIFIER GUID at offset 12.
        constexpr ULONG rpc_handshake_interface_offset = 12;
        if (c.send_buffer_length >= rpc_handshake_interface_offset + this->bound_interface_.size())
        {
            win_emu.emu().read_memory(c.send_buffer + rpc_handshake_interface_offset, this->bound_interface_.data(),
                                      this->bound_interface_.size());
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

        const auto pointer_size = win_emu.process.is_wow64_process ? utils::aligned_binary_writer::pointer_size_32
                                                                   : utils::aligned_binary_writer::pointer_size_64;

        std::vector<uint8_t> payload;
        utils::aligned_binary_writer writer(payload, pointer_size);
        writer.write(header.data(), header.size());

        lpc_request_context rpc_context{};
        rpc_context.send_buffer = c.send_buffer + rpc_call_send_header_size;
        rpc_context.send_buffer_length = c.send_buffer_length - rpc_call_send_header_size;
        rpc_context.recv_buffer = c.recv_buffer + sizeof(header);
        if (c.recv_buffer_length >= header.size())
        {
            rpc_context.recv_buffer_length = c.recv_buffer_length - static_cast<DWORD>(header.size());
        }

        std::vector<alpc_reply_handle> reply_handles;
        const auto status = this->handle_rpc(win_emu, procedure_id, rpc_context, writer, reply_handles);

        if (getenv("EMULATOR_LOG_RPC"))
        {
            std::string hex;
            char tmp[4];
            for (size_t i = sizeof(header); i < payload.size() && i < sizeof(header) + 128; ++i)
            {
                (void)snprintf(tmp, sizeof(tmp), "%02x ", payload[i]);
                hex += tmp;
            }
            win_emu.log.error("[rpc] opnum=%u reply ndr(%zu) handles(%zu): %s\n", procedure_id, payload.size() - sizeof(header),
                              reply_handles.size(), hex.c_str());
        }

        lpc_request_result result{status, std::move(payload)};
        result.handles = std::move(reply_handles);
        return result;
    }

} // namespace sogen
