#include "../std_include.hpp"
#include "../debug_object.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../windows_path.hpp"

namespace sogen
{
    namespace
    {
        constexpr ULONG debug_object_kill_process_on_exit_information = 1;

        handle store_process_handle(process_context& proc, const debug_object::debuggee& debuggee)
        {
            emulator_process process{};
            for (const auto& existing : proc.processes | std::views::values)
            {
                if (existing.process.value == debuggee.process)
                {
                    process.is_wow64_process = existing.is_wow64_process;
                    process.native_environment = existing.native_environment;
                    process.compatibility_environment = existing.compatibility_environment;
                    break;
                }
            }

            process.process = managed_process{debuggee.process};
            process.id = debuggee.process_id;
            return proc.processes.store(std::move(process));
        }

        handle store_thread_handle(process_context& proc, const debug_object::debuggee& debuggee, const uint32_t thread_id)
        {
            managed_process_thread thread{};
            thread.process = managed_process{debuggee.process};
            thread.process_id = debuggee.process_id;
            thread.thread_id = thread_id;
            thread.resume_observed = true;
            return proc.managed_threads.store(std::move(thread));
        }

        // The kernel hands the debugger a handle to each image it reports, which is how debuggers name modules.
        EmulatorTraits<Emu64>::HANDLE open_image_file(windows_emulator& win_emu, const std::string& guest_path)
        {
            if (guest_path.empty())
            {
                return 0;
            }

            const windows_path path{u8_to_u16(guest_path)};
            if (!path.is_absolute())
            {
                return 0;
            }

            auto [io, status] = win_emu.file_sys.open(path, u"rb");
            if (status != STATUS_SUCCESS)
            {
                return 0;
            }

            file f{};
            f.name = u"\\??\\" + path.u16string();
            f.drive_number = static_cast<uint8_t>(path.get_drive().value() - 'a' + 1);
            f.access_mask = GENERIC_READ;
            f.handle = guest_file{win_emu.file_sys, path, u"rb", std::move(io)};
            return win_emu.process.files.store(std::move(f)).bits;
        }

        DBG_STATE exception_state(const uint32_t code)
        {
            if (code == STATUS_BREAKPOINT)
            {
                return DbgBreakpointStateChange;
            }

            if (code == STATUS_SINGLE_STEP)
            {
                return DbgSingleStepStateChange;
            }

            return DbgExceptionStateChange;
        }

        DBGUI_WAIT_STATE_CHANGE64 convert_event(windows_emulator& win_emu, debug_object::debuggee& debuggee,
                                                const process_debug_event& event)
        {
            auto& proc = win_emu.process;
            DBGUI_WAIT_STATE_CHANGE64 change{};
            change.AppClientId.UniqueProcess = debuggee.process_id;
            change.AppClientId.UniqueThread = event.thread_id;

            switch (event.kind)
            {
            case process_debug_event_kind::create_process: {
                change.NewState = DbgCreateProcessStateChange;
                auto& info = change.StateInfo.CreateProcessInfo;
                info.HandleToProcess = store_process_handle(proc, debuggee).bits;
                info.HandleToThread = store_thread_handle(proc, debuggee, event.thread_id).bits;
                info.NewProcess.FileHandle = open_image_file(win_emu, event.path);
                info.NewProcess.BaseOfImage = event.base_address;
                info.NewProcess.InitialThread.StartAddress = event.start_address;
                break;
            }
            case process_debug_event_kind::create_thread:
                change.NewState = DbgCreateThreadStateChange;
                change.StateInfo.CreateThread.HandleToThread = store_thread_handle(proc, debuggee, event.thread_id).bits;
                change.StateInfo.CreateThread.NewThread.StartAddress = event.start_address;
                debuggee.thread_ids.push_back(event.thread_id);
                break;
            case process_debug_event_kind::exit_thread:
                change.NewState = DbgExitThreadStateChange;
                change.StateInfo.ExitThread.ExitStatus = static_cast<NTSTATUS>(event.exit_status);
                std::erase(debuggee.thread_ids, event.thread_id);
                break;
            case process_debug_event_kind::exit_process:
                change.NewState = DbgExitProcessStateChange;
                change.StateInfo.ExitProcess.ExitStatus = static_cast<NTSTATUS>(event.exit_status);
                debuggee.exit_reported = true;
                break;
            case process_debug_event_kind::exception: {
                change.NewState = exception_state(event.exception_code);
                auto& record = change.StateInfo.Exception.ExceptionRecord;
                record.ExceptionCode = event.exception_code;
                record.ExceptionFlags = event.exception_flags;
                record.ExceptionAddress = event.exception_address;
                const auto count = std::min(event.exception_parameters.size(), std::size(record.ExceptionInformation));
                record.NumberParameters = static_cast<DWORD>(count);
                std::copy_n(event.exception_parameters.begin(), count, record.ExceptionInformation);
                change.StateInfo.Exception.FirstChance = event.first_chance ? 1 : 0;
                break;
            }
            case process_debug_event_kind::load_dll:
                change.NewState = DbgLoadDllStateChange;
                change.StateInfo.LoadDll.FileHandle = open_image_file(win_emu, event.path);
                change.StateInfo.LoadDll.BaseOfDll = event.base_address;
                break;
            case process_debug_event_kind::unload_dll:
                change.NewState = DbgUnloadDllStateChange;
                change.StateInfo.UnloadDll.BaseAddress = event.base_address;
                break;
            }

            return change;
        }
    }

    bool deliver_debug_event(windows_emulator& win_emu, const handle debug_handle, const uint64_t state_change)
    {
        auto* object = win_emu.process.debug_objects.get(debug_handle);
        auto* manager = win_emu.processes();
        if (!object || !manager)
        {
            return false;
        }

        for (auto& debuggee : object->debuggees)
        {
            if (debuggee.exit_reported)
            {
                continue;
            }

            const managed_process process{debuggee.process};
            auto event = manager->next_debug_event(process).event;
            if (!event)
            {
                // A child that ended without reporting its exit (killed, or its emulator failed) still ends the session.
                const auto exit = manager->exit_status(process);
                if (!exit || !exit.status)
                {
                    continue;
                }

                event = process_debug_event{
                    .kind = process_debug_event_kind::exit_process,
                    .thread_id = debuggee.thread_ids.empty() ? 0 : debuggee.thread_ids.front(),
                    .exit_status = static_cast<uint32_t>(exit.status->code),
                };
            }

            const auto change = convert_event(win_emu, debuggee, *event);
            win_emu.memory.write_memory(state_change, &change, sizeof(change));
            return true;
        }

        return false;
    }

    namespace syscalls
    {
        NTSTATUS handle_NtCreateDebugObject(const syscall_context& c, const emulator_object<handle> debug_object_handle,
                                            const ACCESS_MASK /*desired_access*/,
                                            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                            const ULONG flags)
        {
            debug_object object{};
            object.kill_on_close = (flags & DEBUG_KILL_ON_CLOSE) != 0;
            debug_object_handle.write(c.proc.debug_objects.store(std::move(object)));
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtWaitForDebugEvent(const syscall_context& c, const handle debug_handle, const BOOLEAN alertable,
                                            const emulator_object<LARGE_INTEGER> timeout, const emulator_pointer state_change)
        {
            if (!c.proc.debug_objects.get(debug_handle))
            {
                return STATUS_INVALID_HANDLE;
            }

            if (deliver_debug_event(c.win_emu, debug_handle, state_change))
            {
                return STATUS_SUCCESS;
            }

            if (timeout && timeout.read().QuadPart == 0)
            {
                return STATUS_TIMEOUT;
            }

            auto& t = c.thread();
            t.await_objects = {};
            t.await_any = false;
            t.await_time = {};
            t.await_msg = {};
            t.waiting_for_alert = false;
            t.await_debug_event = pending_debug_event_wait{.debug_object = debug_handle, .state_change = state_change};
            if (timeout)
            {
                t.await_debug_event->timeout = utils::convert_delay_interval_to_time_point(c.win_emu.clock(), timeout.read());
            }

            c.win_emu.yield_thread(c.vcpu, alertable != FALSE);
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtDebugContinue(const syscall_context& c, const handle debug_handle, const emulator_object<CLIENT_ID64> client_id,
                                        const NTSTATUS continue_status)
        {
            auto* object = c.proc.debug_objects.get(debug_handle);
            auto* manager = c.win_emu.processes();
            if (!object || !manager)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto id = client_id.read();
            const auto debuggee = std::ranges::find(object->debuggees, id.UniqueProcess, &debug_object::debuggee::process_id);
            if (debuggee == object->debuggees.end())
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto error = manager->continue_debug_event(managed_process{debuggee->process}, static_cast<uint32_t>(id.UniqueThread),
                                                             static_cast<uint32_t>(continue_status));

            // The child may be gone by the time its exit is continued.
            if (error != process_error::none && !debuggee->exit_reported)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtSetInformationDebugObject(const syscall_context& c, const handle debug_handle, const ULONG information_class,
                                                    const emulator_pointer information, const ULONG length,
                                                    const emulator_object<ULONG> return_length)
        {
            auto* object = c.proc.debug_objects.get(debug_handle);
            if (!object)
            {
                return STATUS_INVALID_HANDLE;
            }

            if (information_class != debug_object_kill_process_on_exit_information)
            {
                return STATUS_INVALID_PARAMETER;
            }

            return_length.write_if_valid(static_cast<ULONG>(sizeof(ULONG)));
            if (length < sizeof(ULONG))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            object->kill_on_close = (c.emu.read_memory<ULONG>(information) & DEBUG_KILL_ON_CLOSE) != 0;
            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtRemoveProcessDebug(const syscall_context& c, const handle process_handle, const handle debug_handle)
        {
            auto* object = c.proc.debug_objects.get(debug_handle);
            const auto* process = c.proc.processes.get(process_handle);
            if (!object || !process)
            {
                return STATUS_INVALID_HANDLE;
            }

            const auto is_debuggee = [&](const debug_object::debuggee& debuggee) { return debuggee.process == process->process.value; };
            if (std::ranges::none_of(object->debuggees, is_debuggee))
            {
                return STATUS_PORT_NOT_SET;
            }

            if (auto* manager = c.win_emu.processes())
            {
                std::ignore = manager->detach_debugger(process->process);
            }

            std::erase_if(object->debuggees, is_debuggee);
            return STATUS_SUCCESS;
        }
    }
}
