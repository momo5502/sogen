#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

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

        port p{};
        p.name = std::move(port_name);

        if (connection_info)
        {
            std::vector<uint8_t> zero_mem{};
            zero_mem.resize(connection_info_length.read(), 0);
            c.emu.write_memory(connection_info, zero_mem.data(), zero_mem.size());
        }

        client_shared_memory.access([&](PORT_VIEW64& view) {
            p.view_base = c.win_emu.memory.allocate_memory(static_cast<size_t>(view.ViewSize), memory_permission::read_write);
            view.ViewBase = p.view_base;
            view.ViewRemoteBase = view.ViewBase;
        });

        const auto handle = c.proc.ports.store(std::move(p));
        client_port_handle.write(handle);

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

    NTSTATUS handle_NtAlpcSendWaitReceivePort(const syscall_context& c, const handle port_handle, const ULONG /*flags*/,
                                              const emulator_object<PORT_MESSAGE64> /*send_message*/,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*send_message_attributes*/,
                                              const emulator_object<PORT_MESSAGE64> receive_message,
                                              const emulator_object<EmulatorTraits<Emu64>::SIZE_T> /*buffer_length*/,
                                              const emulator_object<ALPC_MESSAGE_ATTRIBUTES>
                                              /*receive_message_attributes*/,
                                              const emulator_object<LARGE_INTEGER> /*timeout*/)
    {
        const auto* port = c.proc.ports.get(port_handle);
        if (!port)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (port->name != u"\\Windows\\ApiPort")
        {
            c.win_emu.log.error("!!! BAD PORT\n");
            return STATUS_NOT_SUPPORTED;
        }

        // TODO: Fix this. This is broken and wrong.

        try
        {
            const emulator_object<PORT_DATA_ENTRY<EmulatorTraits<Emu64>>> data{c.emu, receive_message.value() + 0x48};
            const auto dest = data.read();
            const auto base = dest.Base;

            const auto value = base + 0x10;
            c.emu.write_memory(base + 8, &value, sizeof(value));
        }
        catch (...)
        {
            return STATUS_NOT_SUPPORTED;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAlpcConnectPort()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtAlpcConnectPortEx()
    {
        return STATUS_NOT_SUPPORTED;
    }
}
