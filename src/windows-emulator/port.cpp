#include "std_include.hpp"
#include "port.hpp"
#include "logger.hpp"
#include "io_completion_wait.hpp"
#include "windows_emulator.hpp"
#include "ports/api_port.hpp"
#include "ports/audio_service.hpp"
#include "ports/core_messaging_registrar.hpp"
#include "ports/dns_resolver.hpp"
#include "ports/event_log.hpp"
#include "ports/lsa_policy_lookup.hpp"
#include "ports/local_object_exporter.hpp"
#include "ports/protected_storage.hpp"
#include "ports/service_control.hpp"
#include "ports/sspi_rpc.hpp"
#include "binary_writer.hpp"

#include <platform/unicode.hpp>
#include <utils/finally.hpp>

namespace sogen
{

    namespace
    {
        constexpr uint32_t rpc_call_large_payload_flag = 4;

        struct dummy_port : port
        {
            explicit dummy_port(std::u16string name)
                : name_(std::move(name))
            {
            }

            lpc_request_result handle_request(windows_emulator& win_emu, const lpc_request_context&) override
            {
                (void)win_emu;
                // win_emu.log.error("!!! BAD PORT: %s\n", u16_to_u8(name_).c_str());
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
                    win_emu.process.audio_render_event.store(c.send_handle, std::memory_order_relaxed);
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

    NTSTATUS port::create_section(const uint64_t size, uint64_t& section_handle, uint64_t& actual_size)
    {
        if (size == 0 || size > std::numeric_limits<uint64_t>::max() - (ALLOCATION_GRANULARITY - 1))
        {
            return STATUS_INVALID_PARAMETER;
        }
        actual_size = align_up(size, ALLOCATION_GRANULARITY);
        if (this->next_section_handle > std::numeric_limits<uint64_t>::max() - 4)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        const auto id = this->next_section_handle;
        this->sections.emplace(id, alpc_port_section{.size = actual_size});
        this->next_section_handle += 4;
        section_handle = id;
        return STATUS_SUCCESS;
    }

    NTSTATUS port::delete_section(const uint64_t section_handle)
    {
        const auto section = this->sections.find(section_handle);
        if (section == this->sections.end() || section->second.closed)
        {
            return STATUS_INVALID_HANDLE;
        }
        if (section->second.views.empty())
        {
            this->sections.erase(section);
        }
        else
        {
            section->second.closed = true;
        }
        return STATUS_SUCCESS;
    }

    NTSTATUS port::create_section_view(windows_emulator& win_emu, const uint64_t section_handle, const uint64_t requested_size,
                                       uint64_t& result_base, uint64_t& actual_size)
    {
        const auto section = this->sections.find(section_handle);
        if (section == this->sections.end() || section->second.closed)
        {
            return STATUS_INVALID_HANDLE;
        }
        if (requested_size == 0 || requested_size > std::numeric_limits<uint64_t>::max() - (ALLOCATION_GRANULARITY - 1))
        {
            return STATUS_INVALID_PARAMETER;
        }
        const auto size = page_align_up(requested_size);
        // ALPC reserves allocation-granularity slots even for page-sized views.
        const auto reserved_size = align_up(size, ALLOCATION_GRANULARITY);
        uint64_t offset{};
        for (const auto& [view_offset, view] : section->second.views)
        {
            if (view_offset >= offset && reserved_size <= view_offset - offset)
            {
                break;
            }
            offset = view_offset + view.reserved_size;
        }
        if (offset > section->second.size || reserved_size > section->second.size - offset)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (size > std::numeric_limits<size_t>::max())
        {
            return STATUS_NO_MEMORY;
        }
        const auto start = win_emu.process.is_wow64_process ? DEFAULT_ALLOCATION_ADDRESS_32BIT : 0;
        const auto base = win_emu.memory.allocate_memory(static_cast<size_t>(size), memory_permission::read_write, false, start,
                                                         memory_region_kind::pagefile_section_view);
        if (base == 0)
        {
            return STATUS_NO_MEMORY;
        }
        auto rollback = utils::finally([&] { win_emu.memory.release_memory(base, static_cast<size_t>(size)); });
        for (const auto& [page_offset, page] : section->second.saved_pages)
        {
            if (page_offset >= offset && page_offset - offset < size)
            {
                win_emu.memory.write_memory(base + page_offset - offset, page.data(), page.size());
            }
        }
        section->second.views.emplace(offset, alpc_section_view{.base = base, .size = size, .reserved_size = reserved_size});
        result_base = base;
        actual_size = size;
        rollback.cancel();
        return STATUS_SUCCESS;
    }

    NTSTATUS port::delete_section_view(windows_emulator& win_emu, const uint64_t address)
    {
        for (auto section = this->sections.begin(); section != this->sections.end(); ++section)
        {
            auto& state = section->second;
            const auto view = std::ranges::find_if(state.views, [&](const auto& entry) { return entry.second.base == address; });
            if (view == state.views.end())
            {
                continue;
            }
            if (!state.closed)
            {
                std::map<uint64_t, std::array<uint8_t, 0x1000>> saved;
                for (uint64_t offset = 0; offset < view->second.size; offset += 0x1000)
                {
                    std::array<uint8_t, 0x1000> page{};
                    const auto page_address = address + offset;
                    const auto region = win_emu.memory.get_region_info(page_address);
                    if (!region.is_committed)
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    const auto readable = (region.permissions.common & memory_permission::read) != memory_permission::none &&
                                          !region.permissions.is_guarded();
                    // Direct-mapped backends enforce guest protections even on emulator reads of section backing data.
                    if (!readable && !win_emu.memory.protect_memory(page_address, page.size(), memory_permission::read))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    auto restore_permissions = utils::finally([&] {
                        if (!readable)
                        {
                            win_emu.memory.protect_memory(page_address, page.size(), region.permissions);
                        }
                    });
                    if (!win_emu.memory.try_read_memory(page_address, page.data(), page.size()))
                    {
                        return STATUS_ACCESS_VIOLATION;
                    }
                    saved.emplace(view->first + offset, page);
                }
                for (auto& [offset, page] : saved)
                {
                    const auto existing = state.saved_pages.find(offset);
                    if (existing != state.saved_pages.end())
                    {
                        existing->second = page;
                    }
                }
                state.saved_pages.merge(saved);
            }
            win_emu.memory.release_memory(address, static_cast<size_t>(view->second.size));
            state.views.erase(view);
            if (state.closed && state.views.empty())
            {
                this->sections.erase(section);
            }
            return STATUS_SUCCESS;
        }
        return STATUS_INVALID_ADDRESS;
    }

    bool port::owns_section_view(const uint64_t section_handle, const uint64_t address, const uint64_t size) const
    {
        const auto section = this->sections.find(section_handle);
        if (section == this->sections.end() || section->second.closed || size == 0)
        {
            return false;
        }
        return std::ranges::any_of(section->second.views,
                                   [&](const auto& entry) { return entry.second.base == address && size <= entry.second.size; });
    }

    void port::release_sections(windows_emulator& win_emu)
    {
        for (const auto& section : this->sections | std::views::values)
        {
            for (const auto& view : section.views | std::views::values)
            {
                win_emu.memory.release_memory(view.base, static_cast<size_t>(view.size));
            }
        }
        this->sections.clear();
    }

    std::unique_ptr<port> create_port(const std::u16string_view port)
    {
        if (port == u"\\Windows\\ApiPort")
        {
            return create_api_port();
        }

        if (port == u"\\RPC Control\\protected_storage")
        {
            return create_protected_storage_port();
        }

        if (port == u"\\RPC Control\\DNSResolver")
        {
            return create_dns_resolver();
        }

        if (port == u"\\RPC Control\\LSARPC_ENDPOINT" || port == u"\\RPC Control\\lsapolicylookup")
        {
            return create_lsa_policy_lookup_port();
        }

        if (port == u"\\RPC Control\\eventlog")
        {
            return create_event_log_port();
        }

        if (port == u"\\RPC Control\\epmapper")
        {
            return create_local_object_exporter_port();
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

        if (port == u"\\RPC Control\\lsasspirpc")
        {
            return create_sspi_rpc_port();
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

    void port_container::post_completion(windows_emulator& win_emu) const
    {
        auto* completion = win_emu.process.io_completions.get(this->completion_port_);
        if (!completion)
        {
            return;
        }
        io_completion_message message{};
        message.key_context = this->completion_key_;
        completion->enqueue(message);
    }

    NTSTATUS port_container::associate_completion_port(windows_emulator& win_emu, const emulator_thread* active_thread,
                                                       const uint64_t completion_key, const handle completion_port)
    {
        if (this->completion_port_.bits != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto resolved_port = win_emu.process.resolve_object_pseudo_handle(completion_port, active_thread);
        if (resolved_port.value.type != handle_types::io_completion || !win_emu.process.io_completions.get(resolved_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        handle retained_port{};
        if (!io_completion_wait::retain_handle_reference(win_emu.process, active_thread, resolved_port, retained_port))
        {
            return STATUS_INVALID_HANDLE;
        }

        this->completion_key_ = completion_key;
        this->completion_port_ = retained_port;

        for (size_t index = 0; index < this->reply_queue_.size(); ++index)
        {
            this->post_completion(win_emu);
        }

        return STATUS_SUCCESS;
    }

    void port_container::release_completion_port(process_context& process)
    {
        io_completion_wait::release_handle_reference(process, this->completion_port_);
        this->completion_key_ = 0;
    }

    void port_container::queue_connection_reply(windows_emulator& win_emu, const bool is_wow64)
    {
        lpc_message_result reply{};
        reply.message.is_wow64 = is_wow64;
        reply.message.native.u1.s1.TotalLength = static_cast<CSHORT>(lpc_port_message::wire_size(is_wow64));
        reply.message.native.u2.s2.Type = LPC_CONNECTION_REPLY;
        reply.message.native.MessageId = this->allocate_message_id();

        this->reply_queue_.push_back(std::move(reply));
        this->post_completion(win_emu);
    }

    lpc_message_result port_container::handle_message(windows_emulator& win_emu, const lpc_message_context& c)
    {
        this->assert_validity();

        if (this->port_->disconnected)
        {
            return {.status = STATUS_PORT_DISCONNECTED};
        }

        if (!c.receive_message && !this->port_->accepts_send_only_messages())
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

        if (!c.receive_message)
        {
            if (this->completion_port_.bits == 0 || !NT_SUCCESS(result.status))
            {
                return {.status = result.status};
            }

            const auto status = result.status;
            result.message_context = c.send_message_context;
            reply_queue_.push_back(std::move(result));
            this->post_completion(win_emu);
            return {.status = status};
        }

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
        context.send_view_base = c.send_view_base;
        context.send_view_size = c.send_view_size;

        auto request_result = this->handle_request(win_emu, context);
        const auto payload_size = request_result.payload ? static_cast<ULONG>(request_result.payload->size()) : context.recv_buffer_length;

        if (!request_result.view_payload.empty())
        {
            recv_header.native.u2.s2.Type |= lpc_continuation_required;
        }

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
        result.view_payload = std::move(request_result.view_payload);

        return result;
    }

    lpc_request_result rpc_port::handle_request(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_op_size = sizeof(uint32_t);
        if (c.send_buffer_length < rpc_op_size)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto operation = emulator_object<uint32_t>{win_emu.memory, c.send_buffer}.try_read();
        if (!operation)
        {
            return STATUS_ACCESS_VIOLATION;
        }

        switch (*operation)
        {
        case 1: // Handshake
            return handle_handshake(win_emu, c);
        case 0: // Call
            return handle_rpc_call(win_emu, c);
        default:
            win_emu.log.print(color::gray, "Unexpected RPC operation: 0x%X\n", *operation);
            return STATUS_NOT_SUPPORTED;
        }
    }

    lpc_request_result rpc_port::handle_handshake(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_handshake_state_offset = 32;
        constexpr ULONG rpc_handshake_context_offset = 36;
        constexpr ULONG required_handshake_read_bytes = rpc_handshake_context_offset + sizeof(uint32_t);

        if (c.send_buffer_length < required_handshake_read_bytes || c.recv_buffer_length < c.send_buffer_length)
        {
            return STATUS_INVALID_PARAMETER;
        }

        // The bind carries the target interface's RPC_SYNTAX_IDENTIFIER GUID at offset 12.
        constexpr ULONG rpc_handshake_interface_offset = 12;
        std::vector<uint8_t> payload(c.send_buffer_length, 0);
        if (!win_emu.memory.try_read_memory(c.send_buffer, payload.data(), payload.size()))
        {
            return STATUS_ACCESS_VIOLATION;
        }

        utils::aligned_binary_writer writer(payload);
        writer.write_at<uint32_t>(8, 0);

        uint32_t syntax{};
        std::memcpy(&syntax, payload.data() + rpc_handshake_state_offset, sizeof(syntax));
        if (syntax == 0 || (syntax & ~uint32_t{3}) != 0)
        {
            return STATUS_INVALID_PARAMETER;
        }
        const auto ndr64 = (syntax & 2) != 0;
        std::memcpy(this->bound_interface_.data(), payload.data() + rpc_handshake_interface_offset, this->bound_interface_.size());
        for (const auto proposed_ndr64 : {false, true})
        {
            if ((syntax & (proposed_ndr64 ? 2 : 1)) == 0)
            {
                continue;
            }
            const auto context_offset = rpc_handshake_context_offset + (proposed_ndr64 ? sizeof(uint16_t) : 0);
            uint16_t context_id{};
            std::memcpy(&context_id, payload.data() + context_offset, sizeof(context_id));
            this->bindings_.insert_or_assign(context_id, rpc_binding{.interface_id = this->bound_interface_, .ndr64 = proposed_ndr64});
        }
        if (ndr64)
        {
            writer.write_at<uint32_t>(rpc_handshake_state_offset, 2);
        }

        return {STATUS_SUCCESS, std::move(payload)};
    }

    lpc_request_result rpc_port::handle_rpc_call(windows_emulator& win_emu, const lpc_request_context& c)
    {
        constexpr ULONG rpc_call_send_header_size = 0x40;
        constexpr ULONG rpc_call_immediate_response_header_size = 0x18;
        constexpr ULONG rpc_call_large_response_header_size = 0x20;
        constexpr ULONG rpc_call_max_immediate_ndr_size = 0xF00;
        constexpr ULONG rpc_call_id_offset = 12;
        constexpr ULONG rpc_call_opnum_offset = 20;

        if (c.send_buffer_length < rpc_call_send_header_size)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto call_id = emulator_object<uint32_t>{win_emu.memory, c.send_buffer + rpc_call_id_offset}.try_read();
        const auto procedure_id = emulator_object<uint32_t>{win_emu.memory, c.send_buffer + rpc_call_opnum_offset}.try_read();
        const auto context_id = emulator_object<uint16_t>{win_emu.memory, c.send_buffer + 16}.try_read();
        const auto call_flags = emulator_object<uint32_t>{win_emu.memory, c.send_buffer + 8}.try_read();
        if (!call_id || !procedure_id || !context_id || !call_flags)
        {
            return STATUS_ACCESS_VIOLATION;
        }
        const auto binding = this->bindings_.find(*context_id);
        if (binding == this->bindings_.end())
        {
            return STATUS_INVALID_PARAMETER;
        }
        this->bound_interface_ = binding->second.interface_id;
        const auto pointer_size =
            binding->second.ndr64 ? utils::aligned_binary_writer::pointer_size_64 : utils::aligned_binary_writer::pointer_size_32;

        std::array<uint8_t, rpc_call_immediate_response_header_size> header{};
        header[0] = 3;
        std::memcpy(header.data() + rpc_call_id_offset, &*call_id, sizeof(*call_id));

        std::vector<uint8_t> payload;
        utils::aligned_binary_writer writer(payload, pointer_size);
        writer.write(header.data(), header.size());

        lpc_request_context rpc_context{};
        rpc_context.send_buffer = c.send_buffer + rpc_call_send_header_size;
        rpc_context.send_buffer_length = c.send_buffer_length - rpc_call_send_header_size;
        if ((*call_flags & rpc_call_large_payload_flag) != 0)
        {
            constexpr ULONG rpc_call_view_header_size = rpc_call_send_header_size + sizeof(uint64_t);
            if (c.send_buffer_length < rpc_call_view_header_size || c.send_view_base == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }
            const auto view_length = emulator_object<uint32_t>{win_emu.memory, c.send_buffer + rpc_call_send_header_size}.try_read();
            if (!view_length)
            {
                return STATUS_ACCESS_VIOLATION;
            }
            if (*view_length > c.send_view_size)
            {
                return STATUS_INVALID_PARAMETER;
            }
            rpc_context.send_buffer = c.send_view_base;
            rpc_context.send_buffer_length = *view_length;
        }
        rpc_context.send_handle = c.send_handle;
        rpc_context.recv_buffer = c.recv_buffer + rpc_call_immediate_response_header_size;
        if (c.recv_buffer_length >= rpc_call_immediate_response_header_size)
        {
            rpc_context.recv_buffer_length = c.recv_buffer_length - rpc_call_immediate_response_header_size;
        }

        std::vector<alpc_reply_handle> reply_handles;
        const auto status = this->handle_rpc(win_emu, *procedure_id, rpc_context, writer, reply_handles);

        std::vector<uint8_t> view_payload;
        const auto ndr_size = payload.size() - rpc_call_immediate_response_header_size;
        if (ndr_size > rpc_call_max_immediate_ndr_size)
        {
            view_payload.assign(payload.begin() + rpc_call_immediate_response_header_size, payload.end());
            payload.resize(rpc_call_large_response_header_size);
            utils::aligned_binary_writer header_writer(payload);
            header_writer.write_at<uint32_t>(0x08, rpc_call_large_payload_flag);
            header_writer.write_at<uint32_t>(0x18, static_cast<uint32_t>(ndr_size));
        }

        lpc_request_result result{status, std::move(payload)};
        result.view_payload = std::move(view_payload);

        result.handles = std::move(reply_handles);
        return result;
    }

} // namespace sogen
