#include "../std_include.hpp"
#include "../cpu_context.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/finally.hpp>

namespace syscalls
{
    NTSTATUS handle_NtSetInformationThread(const syscall_context& c, const handle thread_handle, const THREADINFOCLASS info_class,
                                           const uint64_t thread_information, const uint32_t thread_information_length)
    {
        auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == ThreadSchedulerSharedDataSlot || info_class == ThreadBasePriority || info_class == ThreadAffinityMask)
        {
            return STATUS_SUCCESS;
        }

        if (info_class == ThreadHideFromDebugger)
        {
            c.win_emu.callbacks.on_suspicious_activity("Hiding thread from debugger");
            return STATUS_SUCCESS;
        }

        if (info_class == ThreadNameInformation)
        {
            if (thread_information_length != sizeof(THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_NAME_INFORMATION<EmulatorTraits<Emu64>>> info{c.emu, thread_information};
            const auto i = info.read();
            thread->name = read_unicode_string(c.emu, i.ThreadName);

            c.win_emu.callbacks.on_thread_set_name(*thread);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadImpersonationToken)
        {
            if (thread_information_length != sizeof(handle))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<handle> info{c.emu, thread_information};
            info.write(DUMMY_IMPERSONATION_TOKEN);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadZeroTlsCell)
        {
            if (thread_information_length != sizeof(ULONG))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto tls_cell = c.emu.read_memory<ULONG>(thread_information);

            for (const auto& t : c.proc.threads | std::views::values)
            {
                t.teb->access([&](TEB64& teb) {
                    if (tls_cell < TLS_MINIMUM_AVAILABLE)
                    {
                        teb.TlsSlots.arr[tls_cell] = 0;
                    }
                    else if (teb.TlsExpansionSlots)
                    {
                        const emulator_object<emulator_pointer> expansion_slots(c.emu, teb.TlsExpansionSlots);
                        expansion_slots.write(0, tls_cell - TLS_MINIMUM_AVAILABLE);
                    }
                });
            }

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported thread set info class: %X\n", info_class);
        c.emu.stop();
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtQueryInformationThread(const syscall_context& c, const handle thread_handle, const uint32_t info_class,
                                             const uint64_t thread_information, const uint32_t thread_information_length,
                                             const emulator_object<uint32_t> return_length)
    {
        const auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (info_class == ThreadTebInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(THREAD_TEB_INFORMATION));
            }

            if (thread_information_length < sizeof(THREAD_TEB_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const auto teb_info = c.emu.read_memory<THREAD_TEB_INFORMATION>(thread_information);
            const auto data = c.emu.read_memory(thread->teb->value() + teb_info.TebOffset, teb_info.BytesToRead);
            c.emu.write_memory(teb_info.TebInformation, data.data(), data.size());

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadBasicInformation)
        {
            if (return_length)
            {
                return_length.write(sizeof(THREAD_BASIC_INFORMATION64));
            }

            if (thread_information_length < sizeof(THREAD_BASIC_INFORMATION64))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_BASIC_INFORMATION64> info{c.emu, thread_information};
            info.access([&](THREAD_BASIC_INFORMATION64& i) {
                i.TebBaseAddress = thread->teb->value();
                i.ClientId = thread->teb->read().ClientId;
            });

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadAmILastThread)
        {
            if (return_length)
            {
                return_length.write(sizeof(ULONG));
            }

            if (thread_information_length < sizeof(ULONG))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<ULONG> info{c.emu, thread_information};
            info.write(c.proc.threads.size() <= 1);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadQuerySetWin32StartAddress)
        {
            if (return_length)
            {
                return_length.write(sizeof(EmulatorTraits<Emu64>::PVOID));
            }

            if (thread_information_length < sizeof(EmulatorTraits<Emu64>::PVOID))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<EmulatorTraits<Emu64>::PVOID> info{c.emu, thread_information};
            info.write(thread->start_address);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadPerformanceCount)
        {
            if (return_length)
            {
                return_length.write(sizeof(LARGE_INTEGER));
            }

            if (thread_information_length < sizeof(LARGE_INTEGER))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<LARGE_INTEGER> info{c.emu, thread_information};
            info.write({});

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadHideFromDebugger)
        {
            if (return_length)
            {
                return_length.write(sizeof(BOOLEAN));
            }

            if (thread_information_length < sizeof(BOOLEAN))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<BOOLEAN> info{c.emu, thread_information};
            info.write(0);

            return STATUS_SUCCESS;
        }

        if (info_class == ThreadTimes)
        {
            if (return_length)
            {
                return_length.write(sizeof(KERNEL_USER_TIMES));
            }

            if (thread_information_length != sizeof(KERNEL_USER_TIMES))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<KERNEL_USER_TIMES> info{c.emu, thread_information};
            info.write(KERNEL_USER_TIMES{});

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported thread query info class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenThread(const syscall_context&, handle /*thread_handle*/, ACCESS_MASK /*desired_access*/,
                                 emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                 emulator_pointer /*client_id*/)
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenThreadToken(const syscall_context&, const handle thread_handle, const ACCESS_MASK /*desired_access*/,
                                      const BOOLEAN /*open_as_self*/, const emulator_object<handle> token_handle)
    {
        if (thread_handle != CURRENT_THREAD)
        {
            return STATUS_NOT_SUPPORTED;
        }

        token_handle.write(CURRENT_THREAD_TOKEN);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenThreadTokenEx(const syscall_context& c, const handle thread_handle, const ACCESS_MASK desired_access,
                                        const BOOLEAN open_as_self, const ULONG /*handle_attributes*/,
                                        const emulator_object<handle> token_handle)
    {
        return handle_NtOpenThreadToken(c, thread_handle, desired_access, open_as_self, token_handle);
    }

    static void delete_thread_windows(const syscall_context& c, const uint32_t thread_id)
    {
        for (auto i = c.proc.windows.begin(); i != c.proc.windows.end();)
        {
            if (i->second.thread_id != thread_id)
            {
                ++i;
                continue;
            }

            i->second.ref_count = 1;
            i = c.proc.windows.erase(i).first;
        }
    }

    NTSTATUS handle_NtTerminateThread(const syscall_context& c, const handle thread_handle, const NTSTATUS exit_status)
    {
        auto* thread = !thread_handle.bits ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        thread->exit_status = exit_status;
        c.win_emu.callbacks.on_thread_terminated(thread_handle, *thread);

        delete_thread_windows(c, thread->id);

        if (thread == c.proc.active_thread)
        {
            c.win_emu.yield_thread();
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtDelayExecution(const syscall_context& c, const BOOLEAN alertable, const emulator_object<LARGE_INTEGER> delay_interval)
    {
        auto& t = c.win_emu.current_thread();
        t.await_time = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), delay_interval.read());
        c.win_emu.yield_thread(alertable);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtAlertThreadByThreadId(const syscall_context& c, const uint64_t thread_id)
    {
        for (auto& t : c.proc.threads | std::views::values)
        {
            if (t.id == thread_id)
            {
                t.alerted = true;
                return STATUS_SUCCESS;
            }
        }

        return STATUS_INVALID_HANDLE;
    }

    NTSTATUS handle_NtAlertThreadByThreadIdEx(const syscall_context& c, const uint64_t thread_id,
                                              const emulator_object<EMU_RTL_SRWLOCK<EmulatorTraits<Emu64>>> /*lock*/)
    {
        // TODO: Support lock
        /*if (lock.value())
        {
             c.win_emu.log.warn("NtAlertThreadByThreadIdEx with lock not supported yet!\n");
            //  c.emu.stop();
            //  return STATUS_NOT_SUPPORTED;
        }*/

        return handle_NtAlertThreadByThreadId(c, thread_id);
    }

    NTSTATUS handle_NtWaitForAlertByThreadId(const syscall_context& c, const uint64_t, const emulator_object<LARGE_INTEGER> timeout)
    {
        auto& t = c.win_emu.current_thread();
        t.waiting_for_alert = true;

        if (timeout.value() && !t.await_time.has_value())
        {
            t.await_time = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());
        }

        c.win_emu.yield_thread();

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtYieldExecution(const syscall_context& c)
    {
        c.win_emu.yield_thread();
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtResumeThread(const syscall_context& c, const handle thread_handle,
                                   const emulator_object<ULONG> previous_suspend_count)
    {
        auto* thread = c.proc.threads.get(thread_handle);
        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto old_count = thread->suspended;
        if (previous_suspend_count)
        {
            previous_suspend_count.write(old_count);
        }

        if (old_count > 0)
        {
            thread->suspended -= 1;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtContinueEx(const syscall_context& c, const emulator_object<CONTEXT64> thread_context,
                                 const uint64_t continue_argument)
    {
        c.write_status = false;

        KCONTINUE_ARGUMENT argument{};
        if (continue_argument <= 0xFF)
        {
            argument.ContinueFlags = KCONTINUE_FLAG_TEST_ALERT;
        }
        else
        {
            argument = c.emu.read_memory<KCONTINUE_ARGUMENT>(continue_argument);
        }

        const auto context = thread_context.read();
        cpu_context::restore(c.emu, context);

        if (argument.ContinueFlags & KCONTINUE_FLAG_TEST_ALERT)
        {
            c.win_emu.yield_thread(true);
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtContinue(const syscall_context& c, const emulator_object<CONTEXT64> thread_context, const BOOLEAN raise_alert)
    {
        return handle_NtContinueEx(c, thread_context, raise_alert ? 1 : 0);
    }

    NTSTATUS handle_NtGetNextThread(const syscall_context& c, const handle process_handle, const handle thread_handle,
                                    const ACCESS_MASK /*desired_access*/, const ULONG /*handle_attributes*/, const ULONG flags,
                                    const emulator_object<handle> new_thread_handle)
    {
        if (process_handle != CURRENT_PROCESS || thread_handle.value.type != handle_types::thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (flags != 0)
        {
            c.win_emu.log.error("NtGetNextThread flags %X not supported\n", static_cast<uint32_t>(flags));
            c.emu.stop();
            return STATUS_NOT_SUPPORTED;
        }

        bool return_next_thread = thread_handle == NULL_HANDLE;
        for (auto& t : c.proc.threads)
        {
            if (return_next_thread && !t.second.is_terminated())
            {
                ++t.second.ref_count;
                new_thread_handle.write(c.proc.threads.make_handle(t.first));
                return STATUS_SUCCESS;
            }

            if (t.first == thread_handle.value.id)
            {
                return_next_thread = true;
            }
        }

        new_thread_handle.write(NULL_HANDLE);
        return STATUS_NO_MORE_ENTRIES;
    }

    NTSTATUS handle_NtGetContextThread(const syscall_context& c, const handle thread_handle,
                                       const emulator_object<CONTEXT64> thread_context)
    {
        const auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        c.proc.active_thread->save(c.emu);
        const auto _ = utils::finally([&] {
            c.proc.active_thread->restore(c.emu); //
        });

        thread->restore(c.emu);

        thread_context.access([&](CONTEXT64& context) {
            if ((context.ContextFlags & CONTEXT_DEBUG_REGISTERS_64) == CONTEXT_DEBUG_REGISTERS_64)
            {
                c.win_emu.callbacks.on_suspicious_activity("Reading debug registers");
            }

            cpu_context::save(c.emu, context);
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtSetContextThread(const syscall_context& c, const handle thread_handle,
                                       const emulator_object<CONTEXT64> thread_context)
    {
        const auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        const auto needs_swich = thread != c.proc.active_thread;

        if (needs_swich)
        {
            c.proc.active_thread->save(c.emu);
            thread->restore(c.emu);
        }

        const auto _ = utils::finally([&] {
            if (needs_swich)
            {
                c.proc.active_thread->restore(c.emu); //
            }
        });

        const auto context = thread_context.read();
        cpu_context::restore(c.emu, context);

        if ((context.ContextFlags & CONTEXT_DEBUG_REGISTERS_64) == CONTEXT_DEBUG_REGISTERS_64)
        {
            c.win_emu.callbacks.on_suspicious_activity("Setting debug registers");
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtCreateThreadEx(const syscall_context& c, const emulator_object<handle> thread_handle,
                                     const ACCESS_MASK /*desired_access*/,
                                     const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>>
                                     /*object_attributes*/,
                                     const handle process_handle, const uint64_t start_routine, const uint64_t argument,
                                     const ULONG create_flags, const EmulatorTraits<Emu64>::SIZE_T /*zero_bits*/,
                                     const EmulatorTraits<Emu64>::SIZE_T stack_size,
                                     const EmulatorTraits<Emu64>::SIZE_T /*maximum_stack_size*/,
                                     const emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto h = c.proc.create_thread(c.win_emu.memory, start_routine, argument, stack_size, create_flags & CREATE_SUSPENDED);
        thread_handle.write(h);

        if (!attribute_list)
        {
            return STATUS_SUCCESS;
        }

        const auto* thread = c.proc.threads.get(h);

        const emulator_object<PS_ATTRIBUTE<EmulatorTraits<Emu64>>> attributes{
            c.emu, attribute_list.value() + offsetof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>, Attributes)};

        const auto total_length = attribute_list.read().TotalLength;

        constexpr auto entry_size = sizeof(PS_ATTRIBUTE<EmulatorTraits<Emu64>>);
        constexpr auto header_size = sizeof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>) - entry_size;
        const auto attribute_count = (total_length - header_size) / entry_size;

        for (size_t i = 0; i < attribute_count; ++i)
        {
            attributes.access(
                [&](const PS_ATTRIBUTE<EmulatorTraits<Emu64>>& attribute) {
                    const auto type = attribute.Attribute & ~PS_ATTRIBUTE_THREAD;

                    if (type == PsAttributeClientId)
                    {
                        const auto client_id = thread->teb->read().ClientId;
                        write_attribute(c.emu, attribute, client_id);
                    }
                    else if (type == PsAttributeTebAddress)
                    {
                        write_attribute(c.emu, attribute, thread->teb->value());
                    }
                    else
                    {
                        c.win_emu.log.error("Unsupported thread attribute type: %" PRIx64 "\n", type);
                    }
                },
                i);
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtGetCurrentProcessorNumberEx(const syscall_context&, const emulator_object<PROCESSOR_NUMBER> processor_number)
    {
        constexpr PROCESSOR_NUMBER number{};
        processor_number.write(number);
        return STATUS_SUCCESS;
    }

    ULONG handle_NtGetCurrentProcessorNumber()
    {
        return 0;
    }

    NTSTATUS handle_NtQueueApcThreadEx2(const syscall_context& c, const handle thread_handle, const handle /*reserve_handle*/,
                                        const uint32_t apc_flags, const uint64_t apc_routine, const uint64_t apc_argument1,
                                        const uint64_t apc_argument2, const uint64_t apc_argument3)
    {
        auto* thread = thread_handle == CURRENT_THREAD ? c.proc.active_thread : c.proc.threads.get(thread_handle);

        if (!thread)
        {
            return STATUS_INVALID_HANDLE;
        }

        if (apc_flags)
        {
            c.win_emu.log.warn("Unsupported APC flags: %X\n", apc_flags);
            // c.emu.stop();
            // return STATUS_NOT_SUPPORTED;
        }

        thread->pending_apcs.push_back({
            .flags = apc_flags,
            .apc_routine = apc_routine,
            .apc_argument1 = apc_argument1,
            .apc_argument2 = apc_argument2,
            .apc_argument3 = apc_argument3,
        });

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtQueueApcThreadEx(const syscall_context& c, const handle thread_handle, const handle reserve_handle,
                                       const uint64_t apc_routine, const uint64_t apc_argument1, const uint64_t apc_argument2,
                                       const uint64_t apc_argument3)
    {
        uint32_t flags{0};
        auto real_reserve_handle = reserve_handle;
        if (reserve_handle.bits == QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC)
        {
            real_reserve_handle.bits = 0;
            flags = QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC;
            static_assert(QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC == 1);
        }

        return handle_NtQueueApcThreadEx2(c, thread_handle, real_reserve_handle, flags, apc_routine, apc_argument1, apc_argument2,
                                          apc_argument3);
    }

    NTSTATUS handle_NtQueueApcThread(const syscall_context& c, const handle thread_handle, const uint64_t apc_routine,
                                     const uint64_t apc_argument1, const uint64_t apc_argument2, const uint64_t apc_argument3)
    {
        return handle_NtQueueApcThreadEx(c, thread_handle, make_handle(0), apc_routine, apc_argument1, apc_argument2, apc_argument3);
    }
}
