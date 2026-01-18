#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../port.hpp"

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

    NTSTATUS handle_NtAlpcConnectPort(const syscall_context& c, const emulator_object<handle> port_handle,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> server_port_name,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                      const emulator_pointer /*port_attributes*/, const ULONG /*flags*/,
                                      const emulator_pointer /*required_server_sid*/, const emulator_pointer /*connection_message*/,
                                      const emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                      const emulator_pointer /*out_message_attributes*/, const emulator_pointer /*in_message_attributes*/,
                                      const emulator_object<LARGE_INTEGER> /*timeout*/)
    {
        auto port_name = read_unicode_string(c.emu, server_port_name);
        c.win_emu.callbacks.on_generic_access("Connecting port", port_name);

        port_container container{std::u16string(port_name), c.win_emu, {}};

        const auto handle = c.proc.ports.store(std::move(container));
        port_handle.write(handle);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAlpcConnectPortEx(const syscall_context& c, const emulator_object<handle> port_handle,
                                        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> connection_port_object_attributes,
                                        const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*client_port_object_attributes*/,
                                        const emulator_pointer port_attributes, const ULONG flags,
                                        const emulator_pointer /*server_security_requirements*/, const emulator_pointer connection_message,
                                        const emulator_object<EmulatorTraits<Emu64>::SIZE_T> buffer_length,
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

    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, const handle port_handle, const ULONG /*flags*/,
                                              const emulator_object<PORT_MESSAGE64> send_message,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*send_message_attributes*/,
                                              const emulator_object<PORT_MESSAGE64> receive_message,
                                              const emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*receive_message_attributes*/,
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

        return port->handle_message(c.win_emu, context);
    }

    NTSTATUS handle_NtAlpcQueryInformation()
    {
        return STATUS_NOT_SUPPORTED;
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
