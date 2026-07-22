#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../port.hpp"

namespace sogen
{

    namespace syscalls
    {
        NTSTATUS handle_NtConnectPort(const syscall_context& c, const emulator_object<handle> client_port_handle,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                      const emulator_object<SECURITY_QUALITY_OF_SERVICE> /*security_qos*/,
                                      const emulator_object<PORT_VIEW64> client_shared_memory,
                                      const emulator_object<REMOTE_PORT_VIEW64> /*server_shared_memory*/,
                                      const emulator_object<ULONG> /*maximum_message_length*/, const emulator_pointer connection_info,
                                      const emulator_object<ULONG> connection_info_length)
        {
            auto port_name = read_unicode_string(c.emu, server_port_name);
            c.win_emu.callbacks.on_generic_access("Connecting port", port_name);

            port_creation_data data{};
            client_shared_memory.access([&](PORT_VIEW64& view) {
                data.view_size = view.ViewSize;
                data.view_base = c.win_emu.memory.allocate_memory(static_cast<size_t>(data.view_size), memory_permission::read_write);
                view.ViewBase = data.view_base;
                view.ViewRemoteBase = view.ViewBase;
            });

            port_container container{std::u16string(port_name), c.win_emu, data};

            const auto handle = c.proc.ports.store(std::move(container));
            client_port_handle.write(handle);

            if (connection_info)
            {
                std::vector<uint8_t> zero_mem{};
                zero_mem.resize(connection_info_length.read(), 0);
                c.emu.write_memory(connection_info, zero_mem.data(), zero_mem.size());
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSecureConnectPort(const syscall_context& c, emulator_object<handle> client_port_handle,
                                            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                            emulator_object<SECURITY_QUALITY_OF_SERVICE> security_qos,
                                            emulator_object<PORT_VIEW64> client_shared_memory, emulator_pointer /*server_sid*/,
                                            emulator_object<REMOTE_PORT_VIEW64> server_shared_memory,
                                            emulator_object<ULONG> maximum_message_length, emulator_pointer connection_info,
                                            emulator_object<ULONG> connection_info_length)
        {
            return handle_NtConnectPort(c, client_port_handle, server_port_name, security_qos, client_shared_memory, server_shared_memory,
                                        maximum_message_length, connection_info, connection_info_length);
        }

        NTSTATUS handle_NtAlpcCreatePort(const syscall_context& c, const emulator_object<handle> port_handle,
                                         const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                         const emulator_pointer /*port_attributes*/)
        {
            if (!port_handle)
            {
                return STATUS_INVALID_PARAMETER;
            }

            std::u16string port_name{};

            if (object_attributes)
            {
                const auto attributes = object_attributes.read();

                if (attributes.ObjectName)
                {
                    emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name{c.emu, attributes.ObjectName};
                    port_name = std::u16string(read_unicode_string(c.emu, name));
                }
            }

            c.win_emu.callbacks.on_generic_access("Creating ALPC port", port_name);

            port_container container{std::move(port_name), c.win_emu, {}};

            const auto new_handle = c.proc.ports.store(std::move(container));
            port_handle.write(new_handle);

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAlpcConnectPort(const syscall_context& c, const emulator_object<handle> port_handle,
                                          const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                          const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                          const emulator_pointer /*port_attributes*/, const ULONG /*flags*/,
                                          const emulator_pointer /*required_server_sid*/, const emulator_pointer /*connection_message*/,
                                          const emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                          const emulator_pointer /*out_message_attributes*/,
                                          const emulator_pointer /*in_message_attributes*/,
                                          const emulator_object<LARGE_INTEGER> /*timeout*/)
        {
            auto port_name = read_unicode_string(c.emu, server_port_name);
            c.win_emu.callbacks.on_generic_access("Connecting port", port_name);

            port_container container{std::u16string(port_name), c.win_emu, {}};

            const auto handle = c.proc.ports.store(std::move(container));
            port_handle.write(handle);

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAlpcConnectPortEx(
            const syscall_context& c, const emulator_object<handle> port_handle,
            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> connection_port_object_attributes,
            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*client_port_object_attributes*/,
            const emulator_pointer port_attributes, const ULONG flags, const emulator_pointer /*server_security_requirements*/,
            const emulator_pointer connection_message, const emulator_object<EmulatorTraits<Emu64>::SIZE_T> buffer_length,
            const emulator_pointer out_message_attributes, const emulator_pointer in_message_attributes,
            const emulator_object<LARGE_INTEGER> timeout)
        {
            if (!connection_port_object_attributes)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto attributes = connection_port_object_attributes.read();
            if (!attributes.ObjectName)
            {
                return STATUS_INVALID_PARAMETER;
            }

            emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> port_name{c.emu, attributes.ObjectName};
            return handle_NtAlpcConnectPort(c, port_handle, port_name, connection_port_object_attributes, port_attributes, flags, {},
                                            connection_message, buffer_length, out_message_attributes, in_message_attributes, timeout);
        }

        NTSTATUS handle_NtAlpcDisconnectPort(const syscall_context& c, const handle port_handle, const ULONG /*flags*/)
        {
            c.proc.ports.erase(port_handle);
            return STATUS_SUCCESS;
        }

        // Locate the HANDLE attribute inside an ALPC attribute buffer: an 8-byte {Allocated; Valid} header
        // followed by the per-attribute structs, laid out highest-bit-first for the attributes the caller
        // allocated room for. Returns nothing when the caller reserved no room for a handle attribute.
        //
        // The attribute structs preceding the HANDLE one embed pointers/SIZE_T, so their sizes differ between a
        // native 64-bit process and a 32-bit WoW64 one. Placing the attribute at 64-bit offsets for a WoW64
        // client makes rpcrt4 read the handle count/handle from the wrong place, aborting the import (observed
        // as HRESULT_FROM_WIN32 FILE_NOT_FOUND -> AUDCLNT_E_DEVICE_INVALIDATED).
        std::optional<uint64_t> find_handle_attribute(const syscall_context& c, const emulator_object<ALPC_MESSAGE_ATTRIBUTES>& attributes,
                                                      const ULONG allocated_attributes)
        {
            if (!attributes || !(allocated_attributes & ALPC_MESSAGE_HANDLE_ATTRIBUTE))
            {
                return std::nullopt;
            }

            const bool wow64 = c.proc.is_wow64_process;
            uint64_t offset = sizeof(ALPC_MESSAGE_ATTRIBUTES);
            if (allocated_attributes & ALPC_MESSAGE_SECURITY_ATTRIBUTE)
            {
                offset += wow64 ? 0x0C : 0x20;
            }
            if (allocated_attributes & ALPC_MESSAGE_VIEW_ATTRIBUTE)
            {
                offset += wow64 ? 0x10 : 0x20;
            }
            if (allocated_attributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE)
            {
                offset += wow64 ? 0x14 : 0x20;
            }

            return attributes.value() + offset;
        }

        template <typename Traits>
        emulator_object<ALPC_HANDLE_ATTR<Traits>> handle_attribute_at(const syscall_context& c, const uint64_t address)
        {
            return {c.emu, address};
        }

        // Deliver reply handles (e.g. the shared render section in an audio Initialize reply) to the receiver via
        // an ALPC HANDLE message attribute. We only emit a single attribute (the common case for NDR system
        // handles).
        void write_reply_handle_attribute(const syscall_context& c, const emulator_object<ALPC_MESSAGE_ATTRIBUTES>& attributes,
                                          const std::vector<alpc_reply_handle>& handles)
        {
            if (handles.empty())
            {
                return;
            }

            auto header = attributes.read();
            const auto attr_base = find_handle_attribute(c, attributes, header.AllocatedAttributes);
            if (!attr_base)
            {
                return;
            }

            // On a real ALPC receive the KERNEL (not the caller) fills the whole handle attribute: a non-zero
            // Flags value that marks the slot as carrying a duplicated handle, then the Handle/ObjectType/
            // GrantedAccess. A live capture of the audio CreateRemoteStream reply showed Flags=0x001243fb, so we
            // replicate it to match the real kernel's receive layout. rpcrt4 reads ObjectType as the delivered
            // HANDLE COUNT (it fetches each handle via NtAlpcQueryInformationMessage, not from the Handle field).
            constexpr ULONG alpc_received_handle_flags = 0x001243fb & ~0x00040000u; // clear ALPC_HANDLEFLG_INDIRECT
            const auto& h = handles.front();

            const auto write_attribute = [&]<typename Traits>() {
                handle_attribute_at<Traits>(c, *attr_base)
                    .write({
                        .Flags = alpc_received_handle_flags,
                        .Handle = static_cast<typename Traits::HANDLE>(h.handle),
                        .ObjectType = static_cast<ULONG>(handles.size()),
                        .DesiredAccess = h.desired_access,
                    });
            };

            if (c.proc.is_wow64_process)
            {
                write_attribute.template operator()<EmulatorTraits<Emu32>>();
            }
            else
            {
                write_attribute.template operator()<EmulatorTraits<Emu64>>();
            }

            // Report exactly the attributes the reply carries (CONTEXT|HANDLE), matching the real kernel, rather
            // than OR-ing HANDLE onto whatever stale ValidAttributes the caller's buffer happened to contain.
            header.ValidAttributes = (header.ValidAttributes & ALPC_MESSAGE_CONTEXT_ATTRIBUTE) | ALPC_MESSAGE_HANDLE_ATTRIBUTE;
            attributes.write(header);
        }

        // Extract the handle a client sent to the server via an ALPC HANDLE message attribute (e.g. the render
        // event handle in a WASAPI SetEventHandle command). Inverse of write_reply_handle_attribute.
        uint64_t read_send_handle_attribute(const syscall_context& c, const emulator_object<ALPC_MESSAGE_ATTRIBUTES>& attributes)
        {
            if (!attributes)
            {
                return 0;
            }

            const auto attr_base = find_handle_attribute(c, attributes, attributes.read().AllocatedAttributes);
            if (!attr_base)
            {
                return 0;
            }

            return c.proc.is_wow64_process ? handle_attribute_at<EmulatorTraits<Emu32>>(c, *attr_base).read().Handle
                                           : handle_attribute_at<EmulatorTraits<Emu64>>(c, *attr_base).read().Handle;
        }

        NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, const handle port_handle, const ULONG /*flags*/,
                                                  const emulator_object<PORT_MESSAGE64> send_message,
                                                  const emulator_object<ALPC_MESSAGE_ATTRIBUTES> send_message_attributes,
                                                  const emulator_object<PORT_MESSAGE64> receive_message,
                                                  const emulator_object<EmulatorTraits<Emu64>::SIZE_T> buffer_length,
                                                  const emulator_object<ALPC_MESSAGE_ATTRIBUTES> receive_message_attributes,
                                                  const emulator_object<LARGE_INTEGER> /*timeout*/)
        {
            auto* port = c.proc.ports.get(port_handle);
            if (!port)
            {
                return STATUS_INVALID_HANDLE;
            }

            lpc_message_context context{c.emu};
            context.send_message = send_message;
            context.receive_message = receive_message;
            context.receive_buffer_length = buffer_length ? buffer_length.read() : 0;
            context.send_handle = read_send_handle_attribute(c, send_message_attributes);

            const auto result = port->handle_message(c.win_emu, context);

            if (receive_message && NT_SUCCESS(result.status))
            {
                if (context.receive_buffer_length < result.total_length())
                {
                    // The port handler should always queue messages that are too large to be written,
                    // so if we still got a message that doesn't fit in the receive buffer, something
                    // has gone wrong.
                    throw std::runtime_error("Unexpected success result returned by port handler");
                }

                result.message.write(receive_message);

                if (!result.payload.empty())
                {
                    c.emu.write_memory(receive_message.value() + result.message.header_size(), result.payload.data(),
                                       result.payload.size());
                }

                write_reply_handle_attribute(c, receive_message_attributes, result.handles);

                // Stash the delivered handles so rpcrt4 can pull them back via
                // NtAlpcQueryInformationMessage(AlpcMessageHandleInformation), which is how its system-handle
                // unmarshal actually imports them (it does not read the handle attribute's Handle field).
                if (!result.handles.empty())
                {
                    c.proc.pending_alpc_message_handles = result.handles;
                }
            }

            if (receive_message && buffer_length)
            {
                buffer_length.write(static_cast<typename EmulatorTraits<Emu64>::SIZE_T>(result.total_length()));
            }

            return result.status;
        }

        NTSTATUS handle_NtAlpcQueryInformation()
        {
            return STATUS_NOT_SUPPORTED;
        }

        // rpcrt4's client-side system-handle unmarshal imports a handle delivered with an ALPC reply by
        // calling NtAlpcQueryInformationMessage(AlpcMessageHandleInformation = 3, index). The output is an
        // ALPC_MESSAGE_HANDLE_INFORMATION {ULONG Index; ULONG Reserved; ULONG Handle; ULONG ObjectType;
        // ULONG GrantedAccess;} (0x14 bytes); rpcrt4 reads Handle@+8, ObjectType@+0xc, GrantedAccess@+0x10.
        // We return the handle stashed by the matching NtAlpcSendWaitReceivePort reply.
        NTSTATUS handle_NtAlpcQueryInformationMessage(const syscall_context& c, const handle /*port_handle*/,
                                                      const emulator_object<PORT_MESSAGE64> /*port_message*/,
                                                      const uint32_t message_information_class, const emulator_pointer message_information,
                                                      const uint32_t length, const emulator_object<ULONG> return_length)
        {
            constexpr uint32_t alpc_message_handle_information = 3;
            if (message_information_class != alpc_message_handle_information)
            {
                return STATUS_NOT_SUPPORTED;
            }

            constexpr uint32_t info_size = 0x14;
            if (!message_information || length < info_size)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            // The caller passes the requested handle index in the first dword of the buffer.
            const auto index = c.emu.read_memory<uint32_t>(message_information);
            const auto& handles = c.proc.pending_alpc_message_handles;
            if (index >= handles.size())
            {
                return STATUS_NO_MORE_ENTRIES;
            }

            const auto& h = handles[index];
            emulator_object<uint32_t>{c.emu, message_information + 0x00}.write(index);
            emulator_object<uint32_t>{c.emu, message_information + 0x04}.write(0);
            emulator_object<uint32_t>{c.emu, message_information + 0x08}.write(static_cast<uint32_t>(h.handle));
            emulator_object<uint32_t>{c.emu, message_information + 0x0c}.write(h.object_type);
            emulator_object<uint32_t>{c.emu, message_information + 0x10}.write(h.desired_access);

            if (return_length)
            {
                return_length.write(info_size);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAlpcSetInformation()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtAlpcCreateSecurityContext()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtAlpcDeleteSecurityContext()
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtAlpcConnectPortEx()
        {
            return STATUS_NOT_SUPPORTED;
        }
    }

} // namespace sogen
