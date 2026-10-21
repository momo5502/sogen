#include "../std_include.hpp"
#include "../syscall_dispatcher.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <algorithm>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            struct timer_set_parameters
            {
                ULONG version;
                ULONG reserved;
                int64_t no_wake_tolerance;
            };

            std::pair<timer*, NTSTATUS> find_timer(const syscall_context& c, const handle timer_handle, const bool extended)
            {
                const auto effective = c.proc.resolve_object_pseudo_handle(timer_handle, c.vcpu.active_thread);
                auto* store = c.proc.get_handle_store(effective);
                if (!store || !store->get_object(effective))
                {
                    return {nullptr, STATUS_INVALID_HANDLE};
                }
                auto* entry = c.proc.timers.get(effective);
                if (!entry || entry->extended != extended)
                {
                    return {nullptr, STATUS_OBJECT_TYPE_MISMATCH};
                }
                return {entry, STATUS_SUCCESS};
            }

            std::chrono::steady_clock::time_point timer_deadline(utils::clock& clock, const LARGE_INTEGER due)
            {
                const auto now = clock.steady_now();
                uint64_t ticks{};
                if (due.QuadPart <= 0)
                {
                    ticks = static_cast<uint64_t>(-(due.QuadPart + 1)) + 1;
                }
                else
                {
                    const auto system_ticks =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(clock.system_now().time_since_epoch()).count() / 100 +
                        WINDOWS_EPOCH_DIFFERENCE;
                    ticks = due.QuadPart > system_ticks ? static_cast<uint64_t>(due.QuadPart - system_ticks) : 0;
                }
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::time_point::max() - now).count();
                if (ticks > static_cast<uint64_t>(remaining / 100))
                {
                    return std::chrono::steady_clock::time_point::max();
                }
                return now + std::chrono::nanoseconds(ticks * 100);
            }
        }

        // In 100 ns units: the coarsest and finest timer resolutions, and the resolution without this process' request.
        constexpr ULONG coarsest_timer_resolution = 0x0002625a;
        constexpr ULONG finest_timer_resolution = 0x00001388;
        constexpr ULONG default_timer_resolution = 0x00002710;

        // A request can only make the timer finer, never coarser than it already is.
        ULONG current_timer_resolution(const process_context& process)
        {
            return std::min(process.timer_resolution_request.value_or(default_timer_resolution), default_timer_resolution);
        }

        NTSTATUS handle_NtQueryTimerResolution(const syscall_context& c, const emulator_object<ULONG> maximum_time,
                                               const emulator_object<ULONG> minimum_time, const emulator_object<ULONG> current_time)
        {
            maximum_time.write_if_valid(coarsest_timer_resolution);
            minimum_time.write_if_valid(finest_timer_resolution);
            current_time.write_if_valid(current_timer_resolution(c.proc));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetTimerResolution(const syscall_context& c, const ULONG desired_resolution, const BOOLEAN set_resolution,
                                             const emulator_object<ULONG> current_resolution)
        {
            auto status = STATUS_SUCCESS;
            if (set_resolution)
            {
                c.proc.timer_resolution_request = std::clamp(desired_resolution, finest_timer_resolution, coarsest_timer_resolution);
            }
            else if (c.proc.timer_resolution_request)
            {
                c.proc.timer_resolution_request.reset();
            }
            else
            {
                status = STATUS_TIMER_RESOLUTION_NOT_SET;
            }

            current_resolution.write_if_valid(current_timer_resolution(c.proc));
            return status;
        }

        NTSTATUS handle_NtCreateTimer(const syscall_context& c, const emulator_object<handle> timer_handle, ACCESS_MASK /*desired_access*/,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                      const ULONG timer_type)
        {
            if (timer_type > 1)
            {
                return STATUS_INVALID_PARAMETER_4;
            }
            std::u16string name{};
            if (object_attributes)
            {
                const auto attributes = object_attributes.read();
                if (attributes.ObjectName)
                {
                    name = read_unicode_string(c.emu, attributes.ObjectName);
                    c.win_emu.callbacks.on_generic_access("Opening timer", name);
                }
            }

            if (!name.empty())
            {
                for (auto& entry : c.proc.timers)
                {
                    if (entry.second.name == name)
                    {
                        ++entry.second.ref_count;
                        timer_handle.write(c.proc.timers.make_handle(entry.first));
                        return STATUS_OBJECT_NAME_EXISTS;
                    }
                }
            }

            timer t{};
            t.name = std::move(name);
            t.synchronization = timer_type == 1;
            t.inherit = object_attributes && (object_attributes.read().Attributes & OBJ_INHERIT) != 0;

            const auto h = c.proc.timers.store(std::move(t));
            timer_handle.write(h);

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCreateTimer2(const syscall_context& c, const emulator_object<handle> timer_handle, const uint64_t reserved1,
                                       const uint64_t reserved2, const ULONG attributes, ACCESS_MASK /*desired_access*/)
        {
            if (reserved1 != 0 || reserved2 != 0)
            {
                return STATUS_INVALID_PARAMETER;
            }
            if (attributes == 2 || attributes == 6)
            {
                return STATUS_INVALID_PARAMETER;
            }
            constexpr ULONG supported_attributes = 0x4 | 0x8;
            if (attributes & ~supported_attributes)
            {
                return STATUS_INVALID_PARAMETER_4;
            }
            timer entry{};
            entry.extended = true;
            entry.synchronization = true;
            timer_handle.write(c.proc.timers.store(std::move(entry)));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtOpenTimer(const syscall_context& c, const emulator_object<handle> timer_handle, ACCESS_MASK /*desired_access*/,
                                    const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes)
        {
            if (!timer_handle)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            if (!object_attributes)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto attributes = object_attributes.read();

            if (!attributes.ObjectName)
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            auto name = read_unicode_string(c.emu, attributes.ObjectName);
            c.win_emu.callbacks.on_generic_access("Opening timer", name);

            if (name.empty())
            {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }

            for (auto& entry : c.proc.timers)
            {
                if (entry.second.name == name)
                {
                    ++entry.second.ref_count;
                    timer_handle.write(c.proc.timers.make_handle(entry.first));
                    return STATUS_SUCCESS;
                }
            }

            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        NTSTATUS handle_NtSetTimer(const syscall_context& c, const handle timer_handle, const emulator_object<LARGE_INTEGER> due_time,
                                   const uint64_t apc_routine, const uint64_t /*apc_context*/, const BOOLEAN /*resume_timer*/,
                                   const LONG period, const emulator_object<BOOLEAN> previous_state)
        {
            const auto [entry, status] = find_timer(c, timer_handle, false);
            if (!entry)
            {
                return status;
            }
            if (period < 0)
            {
                return STATUS_INVALID_PARAMETER_6;
            }
            if (apc_routine != 0)
            {
                return STATUS_NOT_SUPPORTED;
            }
            const auto due = due_time.read();
            previous_state.write_if_valid(static_cast<BOOLEAN>(entry->is_signaled(c.win_emu.clock().steady_now())));
            entry->due_time = timer_deadline(c.win_emu.clock(), due);
            entry->period_nanoseconds = static_cast<int64_t>(period) * 1000000;
            entry->signaled = false;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetTimer2(const syscall_context& c, const handle timer_handle, const emulator_object<LARGE_INTEGER> due_time,
                                    const emulator_object<LARGE_INTEGER> period, const uint64_t parameters)
        {
            const auto [entry, status] = find_timer(c, timer_handle, true);
            if (!entry)
            {
                return status;
            }
            const auto options = emulator_object<timer_set_parameters>{c.emu, parameters}.read();
            if (options.version != 0 || options.reserved != 0)
            {
                return STATUS_INVALID_PARAMETER_4;
            }
            const auto interval = period ? period.read().QuadPart : 0;
            if (interval < 0 || interval > std::numeric_limits<int64_t>::max() / 100)
            {
                return STATUS_INVALID_PARAMETER_3;
            }
            entry->due_time = timer_deadline(c.win_emu.clock(), due_time.read());
            entry->period_nanoseconds = interval * 100;
            entry->signaled = false;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetTimerEx(const syscall_context& /*c*/, handle /*timer_handle*/, uint32_t /*timer_set_info_class*/,
                                     uint64_t /*timer_set_information*/, ULONG /*timer_set_information_length*/)
        {
            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtQueryTimer(const syscall_context& c, const handle timer_handle, const ULONG information_class,
                                     const uint64_t information, const ULONG information_length, const emulator_object<ULONG> return_length)
        {
            const auto [entry, status] = find_timer(c, timer_handle, false);
            if (!entry)
            {
                return status;
            }
            if (information_class != 0)
            {
                return STATUS_INVALID_INFO_CLASS;
            }

            struct timer_basic_information
            {
                LARGE_INTEGER remaining_time;
                BOOLEAN signaled;
                std::array<uint8_t, 7> reserved;
            };

            return_length.write_if_valid(sizeof(timer_basic_information));
            if (information_length != sizeof(timer_basic_information))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            const auto now = c.win_emu.clock().steady_now();
            timer_basic_information response{};
            response.signaled = static_cast<BOOLEAN>(entry->is_signaled(now));
            if (entry->due_time)
            {
                response.remaining_time.QuadPart =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(*entry->due_time - now).count() / 100;
            }
            c.emu.write_memory(information, response);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCancelTimer(const syscall_context& c, const handle timer_handle, const emulator_object<BOOLEAN> current_state)
        {
            const auto [entry, status] = find_timer(c, timer_handle, false);
            if (!entry)
            {
                return status;
            }
            current_state.write_if_valid(static_cast<BOOLEAN>(entry->is_signaled(c.win_emu.clock().steady_now())));
            entry->cancel();
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtCancelTimer2(const syscall_context& c, const handle timer_handle, uint64_t /*parameters*/)
        {
            const auto [entry, status] = find_timer(c, timer_handle, true);
            if (!entry)
            {
                return status;
            }
            entry->cancel();
            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
