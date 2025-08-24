#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtSetEvent(const syscall_context& c, const uint64_t handle, const emulator_object<LONG> previous_state)
    {
        if (handle == DBWIN_DATA_READY)
        {
            if (c.proc.dbwin_buffer && c.win_emu.callbacks.on_debug_string)
            {
                constexpr auto pid_length = 4;
                const auto debug_data = read_string<char>(c.win_emu.memory, c.proc.dbwin_buffer + pid_length);
                c.win_emu.callbacks.on_debug_string(debug_data);
            }

            return STATUS_SUCCESS;
        }

        auto* entry = c.proc.events.get(handle);
        if (!entry)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (previous_state.value())
        {
            previous_state.write(entry->signaled ? 1ULL : 0ULL);
        }

        entry->signaled = true;
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtTraceEvent()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueryEvent()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtClearEvent(const syscall_context& c, const handle event_handle)
    {
        auto* e = c.proc.events.get(event_handle);
        if (!e)
        {
            return STATUS_INVALID_HANDLE;
        }

        e->signaled = false;
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<handle> event_handle,
                                  const ACCESS_MASK /*desired_access*/,
                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                  const EVENT_TYPE event_type, const BOOLEAN initial_state)
    {
        std::u16string name{};
        if (object_attributes)
        {
            const auto attributes = object_attributes.read();
            if (attributes.ObjectName)
            {
                name = read_unicode_string(c.emu, attributes.ObjectName);
                c.win_emu.callbacks.on_generic_access("Opening event", name);
            }
        }

        if (!name.empty())
        {
            for (auto& entry : c.proc.events)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    event_handle.write(c.proc.events.make_handle(entry.first));
                    return STATUS_OBJECT_NAME_EXISTS;
                }
            }
        }

        event e{};
        e.type = event_type;
        e.signaled = initial_state != FALSE;
        e.name = std::move(name);

        const auto handle = c.proc.events.store(std::move(e));
        event_handle.write(handle);

        static_assert(sizeof(EVENT_TYPE) == sizeof(uint32_t));
        static_assert(sizeof(ACCESS_MASK) == sizeof(uint32_t));

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenEvent(const syscall_context& c, const emulator_object<uint64_t> event_handle,
                                const ACCESS_MASK /*desired_access*/,
                                const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
    {
        const auto attributes = object_attributes.read();
        const auto name = read_unicode_string(c.emu, attributes.ObjectName);
        c.win_emu.callbacks.on_generic_access("Opening event", name);

        if (name == u"\\KernelObjects\\SystemErrorPortReady")
        {
            event_handle.write(WER_PORT_READY.bits);
            return STATUS_SUCCESS;
        }

        if (name == u"Global\\SvcctrlStartEvent_A3752DX")
        {
            event_handle.write(SVCCTRL_START_EVENT.bits);
            return STATUS_SUCCESS;
        }

        if (name == u"\\SECURITY\\LSA_AUTHENTICATION_INITIALIZED")
        {
            event_handle.write(LSA_AUTHENTICATION_INITIALIZED.bits);
            return STATUS_SUCCESS;
        }

        if (name == u"DBWIN_DATA_READY")
        {
            event_handle.write(DBWIN_DATA_READY.bits);
            return STATUS_SUCCESS;
        }

        if (name == u"DBWIN_BUFFER_READY")
        {
            event_handle.write(DBWIN_BUFFER_READY.bits);
            return STATUS_SUCCESS;
        }

        for (auto& entry : c.proc.events)
        {
            if (entry.second.name == name)
            {
                ++entry.second.ref_count;
                event_handle.write(c.proc.events.make_handle(entry.first).bits);
                return STATUS_SUCCESS;
            }
        }

        return STATUS_NOT_FOUND;
    }
}
