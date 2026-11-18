#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "syscall_utils.hpp"

#include <utils/string.hpp>

namespace sogen
{
    namespace syscalls
    {
        void flush_gdi_batch(const syscall_context& c);
    }

    namespace
    {
#ifdef _WIN32
        // Under /EHa a structured exception, such as a host access violation, reaches catch (...) only after the frames
        // that raised it are gone. The filter records where it happened while they are still on the stack.
        struct host_fault
        {
            bool captured{};
            DWORD code{};
            const void* address{};
            std::array<ULONG_PTR, EXCEPTION_MAXIMUM_PARAMETERS> information{};
            DWORD information_count{};
            std::array<void*, 48> frames{};
            USHORT frame_count{};
        };

        int record_host_fault(const EXCEPTION_POINTERS* pointers, host_fault& fault)
        {
            constexpr DWORD cpp_exception_code = 0xE06D7363;
            const auto& record = *pointers->ExceptionRecord;
            if (record.ExceptionCode != cpp_exception_code)
            {
                fault.captured = true;
                fault.code = record.ExceptionCode;
                fault.address = record.ExceptionAddress;
                fault.information_count = std::min<DWORD>(record.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
                std::copy_n(record.ExceptionInformation, fault.information_count, fault.information.begin());
                fault.frame_count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(fault.frames.size()), fault.frames.data(), nullptr);
            }

            return EXCEPTION_CONTINUE_SEARCH;
        }

        void run_recording_host_faults(void (*const body)(const void*), const void* const argument, host_fault& fault)
        {
            __try
            {
                body(argument);
            }
            __except (record_host_fault(GetExceptionInformation(), fault))
            {
            }
        }

        std::string describe_host_address(const void* address)
        {
            HMODULE module{};
            std::array<char, MAX_PATH> path{};
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(address), &module) ||
                GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size())) == 0)
            {
                return "0x" + utils::string::to_hex_number(reinterpret_cast<uintptr_t>(address));
            }

            const auto offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
            return std::filesystem::path(path.data()).filename().string() + "+0x" + utils::string::to_hex_number(offset);
        }

        std::string describe_host_fault(const host_fault& fault)
        {
            std::string description =
                "host exception 0x" + utils::string::to_hex_number(fault.code) + " at " + describe_host_address(fault.address);
            for (DWORD i = 0; i < fault.information_count; ++i)
            {
                description += (i == 0 ? " info " : " ") + std::string("0x") + utils::string::to_hex_number(fault.information[i]);
            }
            description += ", stack:";
            for (USHORT i = 0; i < fault.frame_count; ++i)
            {
                description += " " + describe_host_address(fault.frames[i]);
            }
            return description;
        }
#else
        struct host_fault
        {
        };

        void run_recording_host_faults(void (*const body)(const void*), const void* const argument, host_fault&)
        {
            body(argument);
        }
#endif
    }

    static void serialize(utils::buffer_serializer& buffer, const syscall_handler_entry& obj)
    {
        buffer.write(obj.name);
    }

    static void deserialize(utils::buffer_deserializer& buffer, syscall_handler_entry& obj)
    {
        buffer.read(obj.name);
        obj.handler = nullptr;
    }

    void syscall_dispatcher::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->handlers_);
    }

    void syscall_dispatcher::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_map(this->handlers_);
        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::setup(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                   const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data,
                                   const arch_syscall_abi& abi)
    {
        this->handlers_ = {};

        const auto ntdll_syscalls = find_syscalls(ntdll_exports, ntdll_data, abi);
        const auto win32u_syscalls = find_syscalls(win32u_exports, win32u_data, abi);

        map_syscalls(this->handlers_, ntdll_syscalls);
        map_syscalls(this->handlers_, win32u_syscalls);

        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::add_handlers()
    {
        std::map<std::string, syscall_handler> handler_mapping{};
        syscall_dispatcher::add_handlers(handler_mapping);

        for (auto& entry : this->handlers_ | std::views::values)
        {
            const auto handler = handler_mapping.find(entry.name);
            if (handler == handler_mapping.end())
            {
                continue;
            }

            entry.handler = handler->second;

#ifndef NDEBUG
            handler_mapping.erase(handler);
#endif
        }
    }

    void syscall_dispatcher::dispatch(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        constexpr uint32_t k_win32k_service_table = 0x1000;

        const scheduler_clock::scope time_scope{win_emu.scheduling_clock()};
        auto& emu = vcpu.cpu;
        auto& context = win_emu.process;

        const auto address = emu.read_instruction_pointer();
        const auto& abi = win_emu.abi();
        const auto raw_syscall_id = abi.raw_syscall_id(emu);
        const auto syscall_id = abi.syscall_id(raw_syscall_id);

        const auto entry = this->handlers_.find(syscall_id);
        const auto* syscall_name = (entry != this->handlers_.end()) ? entry->second.name.c_str() : "<unknown>";

        const syscall_context c{
            .win_emu = win_emu,
            .emu = emu,
            .vcpu = vcpu,
            .proc = context,
            .write_status = true,
            .trap_stack_pointer = emu.read_stack_pointer(),
        };

        host_fault fault{};
        try
        {
            const auto body = [&] {
                if (entry == this->handlers_.end())
                {
                    win_emu.log.error("Unknown syscall: 0x%X (raw: 0x%X)\n", syscall_id, raw_syscall_id);
                    win_emu.record_stop(stop_reason::unknown_syscall, "0x" + utils::string::to_hex_number(syscall_id));
                    abi.return_value(c.emu, STATUS_NOT_SUPPORTED);
                    win_emu.stop();
                    return;
                }

                auto& thread = vcpu.thread();
                if (std::exchange(thread.retriggered_syscall_ip, std::nullopt) != address)
                {
                    const auto res = win_emu.callbacks.on_syscall(syscall_id, entry->second.name);
                    if (res == instruction_hook_continuation::skip_instruction)
                    {
                        return;
                    }
                }

                if (!entry->second.handler)
                {
                    win_emu.log.error("Unimplemented syscall: %s - 0x%X (raw: 0x%X)\n", entry->second.name.c_str(), syscall_id,
                                      raw_syscall_id);
                    win_emu.record_stop(stop_reason::unimplemented_syscall, entry->second.name);
                    abi.return_value(c.emu, STATUS_NOT_SUPPORTED);
                    win_emu.stop();
                    return;
                }

                // Like KiSystemCall64, a win32k service first runs the GDI calls gdi32 batched in the TEB, so their output
                // is in place before anything the service does. Apps rely on this to read DIB sections they just drew to.
                if ((syscall_id & k_win32k_service_table) != 0)
                {
                    syscalls::flush_gdi_batch(c);
                }

                entry->second.handler(c);

                if (c.retrigger_syscall)
                {
                    thread.retriggered_syscall_ip = address;
                }
                else
                {
                    dispatch_callback(win_emu, entry->second.name);
                }
            };
            using body_type = decltype(body);
            run_recording_host_faults([](const void* argument) { (*static_cast<const body_type*>(argument))(); }, &body, fault);
        }
        catch (std::exception& e)
        {
            win_emu.log.error("Syscall %s threw an exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ") - %s\n", syscall_name, syscall_id,
                              raw_syscall_id, address, e.what());
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": " + e.what());
            abi.return_value(emu, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
        catch (...)
        {
            win_emu.log.error("Syscall %s threw an unknown exception: 0x%X (raw: 0x%X) (0x%" PRIx64 ")\n", syscall_name, syscall_id,
                              raw_syscall_id, address);
#ifdef _WIN32
            if (fault.captured)
            {
                win_emu.log.error("Syscall %s: %s\n", syscall_name, describe_host_fault(fault).c_str());
            }
#endif
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": <unknown exception>");
            abi.return_value(emu, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
    }

    void syscall_dispatcher::dispatch_callback(windows_emulator& win_emu, std::string& syscall_name)
    {
        // active_cpu(), not emu(): this runs under the syscall's scoped_dispatch, and with more than one
        // vCPU the instrumentation-callback redirect must rewrite the acting vCPU's RIP/r10, not vCPU 0's.
        auto& emu = win_emu.active_cpu();
        auto& context = win_emu.process;

        if (context.instrumentation_callback != 0 && syscall_name != "NtContinue")
        {
            win_emu.abi().redirect_instrumentation_callback(emu, context.instrumentation_callback);
        }
    }

    dispatch_result syscall_dispatcher::dispatch_completion(windows_emulator& win_emu, vcpu_context& vcpu, callback_id callback_id,
                                                            completion_state* completion_state, const user_callback_result& callback_result,
                                                            const uint64_t trap_stack_pointer)
    {
        auto& emu = vcpu.cpu;

        const syscall_context c{.win_emu = win_emu,
                                .emu = emu,
                                .vcpu = vcpu,
                                .proc = win_emu.process,
                                .write_status = true,
                                .is_callback_completion = true,
                                .current_completion_state = completion_state,
                                .previous_callback_result = callback_result,
                                .trap_stack_pointer = trap_stack_pointer};

        const auto entry = this->completion_handlers_.find(callback_id);

        if (entry == this->completion_handlers_.end())
        {
            win_emu.log.error("Unknown callback: 0x%X\n", static_cast<uint32_t>(callback_id));
            win_emu.stop();
            return dispatch_result::error;
        }

        try
        {
            entry->second(c);
            return c.run_callback ? dispatch_result::new_callback : dispatch_result::completed;
        }
        catch (std::exception& e)
        {
            win_emu.log.error("Completion for callback 0x%X threw an exception - %s\n", static_cast<int>(callback_id), e.what());
            win_emu.stop();
            return dispatch_result::error;
        }
        catch (...)
        {
            win_emu.log.error("Completion for callback 0x%X threw an unknown exception\n", static_cast<int>(callback_id));
            win_emu.stop();
            return dispatch_result::error;
        }
    }

    syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                           const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
    {
        this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
    }

} // namespace sogen
