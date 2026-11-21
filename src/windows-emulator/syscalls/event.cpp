#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../memory_condition_events.hpp"

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            std::optional<bool> memory_condition_state(const std::u16string_view name)
            {
                for (const auto& condition : memory_condition_events)
                {
                    if (condition.name == name)
                    {
                        return condition.signaled;
                    }
                }

                return std::nullopt;
            }
        }

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

            const auto was_signaled = c.win_emu.signal_event(*entry);
            if (previous_state.value())
            {
                previous_state.write(was_signaled ? 1ULL : 0ULL);
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtPulseEvent(const syscall_context& c, const uint64_t handle, const emulator_object<LONG> previous_state)
        {
            auto* entry = c.proc.events.get(handle);
            if (!entry)
            {
                return STATUS_INVALID_HANDLE;
            }

            // Threads that are not waiting right now miss the pulse, matching the lossy NtPulseEvent semantics.
            const auto was_signaled = c.win_emu.signal_event(*entry);
            entry->set_signaled(false);
            if (previous_state.value())
            {
                previous_state.write(was_signaled ? 1ULL : 0ULL);
            }
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtTraceEvent()
        {
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryEvent(const syscall_context& c, const handle event_handle, const uint32_t event_information_class,
                                     const emulator_object<EVENT_BASIC_INFORMATION> event_information,
                                     const uint32_t event_information_length, const emulator_object<uint32_t> return_length)
        {
            if (event_information_class != 0) // EventBasicInformation
            {
                return STATUS_INVALID_INFO_CLASS;
            }

            if (event_information_length != sizeof(EVENT_BASIC_INFORMATION))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            EVENT_TYPE type = NotificationEvent;
            bool is_signaled = false;

            if (auto* entry = c.proc.events.get(event_handle))
            {
                type = entry->type;
                is_signaled = entry->is_signaled();
            }

            is_signaled = is_signaled || event_handle == LSA_AUTHENTICATION_INITIALIZED;

            event_information.access([&](EVENT_BASIC_INFORMATION& info) {
                info.EventType = type;
                info.EventState = is_signaled ? 1 : 0;
            });

            if (return_length)
            {
                return_length.write(sizeof(EVENT_BASIC_INFORMATION));
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtClearEvent(const syscall_context& c, const handle event_handle)
        {
            auto* e = c.proc.events.get(event_handle);
            if (!e)
            {
                return STATUS_INVALID_HANDLE;
            }

            e->set_signaled(false);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtResetEvent(const syscall_context& c, const handle event_handle, const emulator_object<LONG> previous_state)
        {
            if (const auto status = check_handle_type(event_handle, handle_types::event); status != STATUS_SUCCESS)
            {
                return status;
            }

            auto* e = c.proc.events.get(event_handle);
            if (!e)
            {
                return STATUS_INVALID_HANDLE;
            }

            previous_state.write_if_valid(e->set_signaled(false) ? 1 : 0);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCreateEvent(const syscall_context& c, const emulator_object<handle> event_handle,
                                      const ACCESS_MASK /*desired_access*/,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                      const EVENT_TYPE event_type, const BOOLEAN initial_state)
        {
            if (const auto status = validate_object_attributes(object_attributes); status != STATUS_SUCCESS)
            {
                return status;
            }

            if (event_type != NotificationEvent && event_type != SynchronizationEvent)
            {
                return STATUS_INVALID_PARAMETER;
            }

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
                        event_handle.write(c.proc.events.open_by_index(entry.first, get_handle_attributes(object_attributes)));
                        return STATUS_OBJECT_NAME_EXISTS;
                    }
                }
            }

            event e{};
            e.type = event_type;
            e.set_signaled(initial_state != FALSE);
            e.name = std::move(name);
            e.inherit = object_attributes && (object_attributes.read().Attributes & OBJ_INHERIT) != 0;

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
            if (const auto status = validate_object_attributes(object_attributes); status != STATUS_SUCCESS)
            {
                return status;
            }

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
                    event_handle.write(c.proc.events.open_by_index(entry.first, get_handle_attributes(object_attributes)).bits);
                    return STATUS_SUCCESS;
                }
            }

            if (const auto signaled = memory_condition_state(name))
            {
                event e{};
                e.type = NotificationEvent;
                e.set_signaled(*signaled);
                e.name = name;
                const auto handle = c.proc.events.store(std::move(e));
                (void)c.proc.events.set_attributes(handle, get_handle_attributes(object_attributes));
                event_handle.write(handle.bits);
                return STATUS_SUCCESS;
            }

            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

} // namespace sogen
