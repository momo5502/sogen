#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/finally.hpp>

namespace syscalls
{
    NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, const handle process_handle, const uint32_t info_class,
                                              const uint64_t process_information, const uint32_t process_information_length,
                                              const emulator_object<uint32_t> return_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        const auto return_length_info = c.win_emu.memory.get_region_info(return_length.value());

        switch (info_class)
        {
        case ProcessExecuteFlags:
            return STATUS_NOT_SUPPORTED;
        case ProcessGroupInformation:
        case ProcessMitigationPolicy: {
            // ProcessMitigationPolicy requires special handling because the caller
            // specifies which policy to query via the Policy field in the input buffer.
            // We need to read this field first to determine what's being queried.

            // Ensure we have at least enough space to read the Policy field
            if (process_information_length < sizeof(PROCESS_MITIGATION_POLICY))
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            // Read the policy type from the input buffer using safe emulator memory access
            const emulator_object<PROCESS_MITIGATION_POLICY> policy_obj{c.emu, process_information};
            const auto policy = policy_obj.read();

            // We only support querying ProcessDynamicCodePolicy
            if (policy != ProcessDynamicCodePolicy)
            {
                return STATUS_NOT_SUPPORTED;
            }

            return handle_query<PROCESS_MITIGATION_POLICY_RAW_DATA>(c.emu, process_information, process_information_length, return_length,
                                                                    [policy](PROCESS_MITIGATION_POLICY_RAW_DATA& policy_data) {
                                                                        policy_data.Policy = policy;
                                                                        policy_data.Value = 0;
                                                                    });
        }
        case ProcessEnclaveInformation:
            return STATUS_NOT_SUPPORTED;

        case ProcessTimes:
            return handle_query<KERNEL_USER_TIMES>(c.emu, process_information, process_information_length, return_length,
                                                   [](KERNEL_USER_TIMES& t) {
                                                       t = {}; //
                                                   });

        case ProcessCookie:
            return handle_query<uint32_t>(c.emu, process_information, process_information_length, return_length, [](uint32_t& cookie) {
                cookie = 0x01234567; //
            });

        case ProcessDebugObjectHandle:

            c.win_emu.callbacks.on_suspicious_activity("Anti-debug check with ProcessDebugObjectHandle");

            if ((process_information & 3) != 0)
            {
                return STATUS_DATATYPE_MISALIGNMENT;
            }

            if (return_length.value() == 0)
            {
                return STATUS_PORT_NOT_SET;
            }

            if (!return_length_info.is_reserved)
            {
                return STATUS_ACCESS_VIOLATION;
            }

            return handle_query<handle>(c.emu, process_information, process_information_length, return_length, [](handle& h) {
                h = NULL_HANDLE;
                return STATUS_PORT_NOT_SET;
            });

        case ProcessDebugFlags:
        case ProcessWx86Information:
        case ProcessDefaultHardErrorMode:
            return handle_query<ULONG>(c.emu, process_information, process_information_length, return_length, [&](ULONG& res) {
                res = (info_class == ProcessDebugFlags ? 1 : 0); //
            });

        case ProcessDebugPort:
            c.win_emu.callbacks.on_suspicious_activity("Anti-debug check with ProcessDebugPort");

            return handle_query<EmulatorTraits<Emu64>::PVOID>(c.emu, process_information, process_information_length, return_length,
                                                              [](EmulatorTraits<Emu64>::PVOID& ptr) {
                                                                  ptr = 0; //
                                                              });

        case ProcessDeviceMap:
            return handle_query<EmulatorTraits<Emu64>::PVOID>(c.emu, process_information, process_information_length, return_length,
                                                              [](EmulatorTraits<Emu64>::PVOID& ptr) {
                                                                  ptr = 0; //
                                                              });

        case ProcessEnableAlignmentFaultFixup:
            return handle_query<BOOLEAN>(c.emu, process_information, process_information_length, return_length, [](BOOLEAN& b) {
                b = FALSE; //
            });

        case ProcessPriorityClass:
            return handle_query<PROCESS_PRIORITY_CLASS>(c.emu, process_information, process_information_length, return_length,
                                                        [](PROCESS_PRIORITY_CLASS& c) {
                                                            c.Foreground = 1;
                                                            c.PriorityClass = 32; // Normal
                                                        });

        case ProcessBasicInformation: {
            const auto init_basic_info = [&](PROCESS_BASIC_INFORMATION64& basic_info) {
                basic_info.PebBaseAddress = c.proc.peb64.value();
                basic_info.UniqueProcessId = 1;
            };

            switch (process_information_length)
            {
            case sizeof(PROCESS_BASIC_INFORMATION64):
                return handle_query<PROCESS_BASIC_INFORMATION64>(c.emu, process_information, process_information_length, return_length,
                                                                 init_basic_info);
            case sizeof(PROCESS_EXTENDED_BASIC_INFORMATION):
                return handle_query<PROCESS_EXTENDED_BASIC_INFORMATION>(
                    c.emu, process_information, process_information_length, return_length,
                    [&](PROCESS_EXTENDED_BASIC_INFORMATION& ext_basic_info) {
                        ext_basic_info.Size = sizeof(PROCESS_EXTENDED_BASIC_INFORMATION);
                        init_basic_info(ext_basic_info.BasicInfo);
                    });
            default:
                return STATUS_INFO_LENGTH_MISMATCH;
            }
        }

        case ProcessImageInformation:
            return handle_query<SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>>(
                c.emu, process_information, process_information_length, return_length,
                [&](SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>& i) {
                    const auto& mod = *c.win_emu.mod_manager.executable;

                    const emulator_object<PEDosHeader_t> dos_header_obj{c.emu, mod.image_base};
                    const auto dos_header = dos_header_obj.read();

                    const emulator_object<PENTHeaders_t<uint64_t>> nt_headers_obj{c.emu, mod.image_base + dos_header.e_lfanew};
                    const auto nt_headers = nt_headers_obj.read();

                    const auto& file_header = nt_headers.FileHeader;
                    const auto& optional_header = nt_headers.OptionalHeader;

                    i.TransferAddress = 0;
                    i.MaximumStackSize = optional_header.SizeOfStackReserve;
                    i.CommittedStackSize = optional_header.SizeOfStackCommit;
                    i.SubSystemType = optional_header.Subsystem;
                    i.SubSystemMajorVersion = optional_header.MajorSubsystemVersion;
                    i.SubSystemMinorVersion = optional_header.MinorSubsystemVersion;
                    i.MajorOperatingSystemVersion = optional_header.MajorOperatingSystemVersion;
                    i.MinorOperatingSystemVersion = optional_header.MinorOperatingSystemVersion;
                    i.ImageCharacteristics = file_header.Characteristics;
                    i.DllCharacteristics = optional_header.DllCharacteristics;
                    i.Machine = file_header.Machine;
                    i.ImageContainsCode = TRUE;
                    i.ImageFlags = 0; // TODO
                    i.ImageFileSize = optional_header.SizeOfImage;
                    i.LoaderFlags = optional_header.LoaderFlags;
                    i.CheckSum = optional_header.CheckSum;
                });

        case ProcessImageFileNameWin32: {
            const auto peb = c.proc.peb64.read();
            emulator_object<RTL_USER_PROCESS_PARAMETERS64> proc_params{c.emu, peb.ProcessParameters};
            const auto params = proc_params.read();
            const auto length = params.ImagePathName.Length + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + 2;

            if (return_length)
            {
                return_length.write(static_cast<uint32_t>(length));
            }

            if (process_information_length < length)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> info{c.emu, process_information};
            info.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
                const auto buffer_start = static_cast<uint64_t>(process_information) + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
                const auto string = read_unicode_string(c.emu, params.ImagePathName);
                c.emu.write_memory(buffer_start, string.c_str(), (string.size() + 1) * 2);
                str.Length = params.ImagePathName.Length;
                str.MaximumLength = str.Length;
                str.Buffer = buffer_start;
            });

            return STATUS_SUCCESS;
        }

        default:
            c.win_emu.log.error("Unsupported process info class: %X\n", info_class);
            c.emu.stop();

            return STATUS_NOT_SUPPORTED;
        }
    }

    NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, const handle process_handle, const uint32_t info_class,
                                            const uint64_t process_information, const uint32_t process_information_length)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == ProcessSchedulerSharedData                     //
            || info_class == ProcessConsoleHostProcess                   //
            || info_class == ProcessFaultInformation                     //
            || info_class == ProcessDefaultHardErrorMode                 //
            || info_class == ProcessRaiseUMExceptionOnInvalidHandleClose //
            || info_class == ProcessDynamicFunctionTableInformation      //
            || info_class == ProcessPriorityBoost                        //
            || info_class == ProcessPriorityClassEx                      //
            || info_class == ProcessPriorityClass || info_class == ProcessAffinityMask)
        {
            return STATUS_SUCCESS;
        }

        if (info_class == ProcessExecuteFlags)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (info_class == ProcessTlsInformation)
        {
            constexpr auto thread_data_offset = offsetof(PROCESS_TLS_INFORMATION, ThreadData);
            const auto total_thread_data_size = process_information_length - thread_data_offset;

            if (process_information_length < sizeof(PROCESS_TLS_INFORMATION) || total_thread_data_size % sizeof(THREAD_TLS_INFORMATION))
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            PROCESS_TLS_INFORMATION tls_info{};
            c.emu.read_memory(process_information, &tls_info, thread_data_offset);

            if (tls_info.OperationType >= MaxProcessTlsOperation || tls_info.Flags & ~PROCESS_TLS_FLAG_VALID_MASK ||
                tls_info.ThreadDataCount == 0 || total_thread_data_size / sizeof(THREAD_TLS_INFORMATION) != tls_info.ThreadDataCount)
            {
                return STATUS_INFO_LENGTH_MISMATCH;
            }

            auto use_teb32 = false;

            if (tls_info.Flags & PROCESS_TLS_FLAG_USE_TEB32)
            {
                if (!c.win_emu.process.is_wow64_process)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                use_teb32 = true;
            }

            const emulator_object<THREAD_TLS_INFORMATION> data{c.emu, process_information + thread_data_offset};

            for (uint32_t i = 0; i < tls_info.ThreadDataCount; i++)
            {
                const auto entry = data.read(i);

                if (entry.Flags)
                {
                    return STATUS_INVALID_PARAMETER;
                }
            }

            for (size_t i = 0; const auto& cur_thread : c.proc.threads | std::views::values)
            {
                if (cur_thread.is_terminated())
                {
                    continue;
                }

                if (i >= tls_info.ThreadDataCount)
                {
                    break;
                }

                size_t pointer_size{};
                uint64_t tls_vector{};
                uint64_t tls_vector_address{};

                if (use_teb32)
                {
                    pointer_size = sizeof(EmulatorTraits<Emu32>::PVOID);
                    tls_vector_address = cur_thread.teb32->value() + offsetof(TEB32, ThreadLocalStoragePointer);
                    cur_thread.teb32->access([&tls_vector](const TEB32& teb32) { tls_vector = teb32.ThreadLocalStoragePointer; });
                }
                else
                {
                    pointer_size = sizeof(EmulatorTraits<Emu64>::PVOID);
                    tls_vector_address = cur_thread.teb64->value() + offsetof(TEB64, ThreadLocalStoragePointer);
                    cur_thread.teb64->access([&tls_vector](const TEB64& teb64) { tls_vector = teb64.ThreadLocalStoragePointer; });
                }

                if (!tls_vector)
                {
                    continue;
                }

                uint64_t previous_tls_vector = tls_vector;
                auto entry = data.read(i);

                if (tls_info.OperationType == ProcessTlsReplaceVector)
                {
                    const auto new_tls_vector = entry.NewTlsData;

                    if (tls_vector == tls_vector_address)
                    {
                        previous_tls_vector = 0;
                    }
                    else
                    {
                        if ((pointer_size - 1) & tls_vector)
                        {
                            return STATUS_DATATYPE_MISALIGNMENT;
                        }

                        c.emu.move_memory(new_tls_vector, tls_vector, pointer_size * tls_info.PreviousCount);
                    }

                    if (use_teb32)
                    {
                        cur_thread.teb32->access(
                            [&new_tls_vector](TEB32& teb32) { teb32.ThreadLocalStoragePointer = static_cast<uint32_t>(new_tls_vector); });
                    }
                    else
                    {
                        cur_thread.teb64->access([&new_tls_vector](TEB64& teb64) { teb64.ThreadLocalStoragePointer = new_tls_vector; });
                    }

                    cur_thread.teb64->access([&entry](TEB64& teb64) { entry.ThreadId = teb64.ClientId.UniqueThread; });
                    entry.OldTlsData = previous_tls_vector;
                }
                else if (tls_info.OperationType == ProcessTlsReplaceIndex)
                {
                    const auto tls_entry_ptr = tls_vector + (tls_info.TlsIndex * pointer_size);
                    uint64_t old_entry{};

                    if (use_teb32)
                    {
                        old_entry = c.emu.read_memory<EmulatorTraits<Emu32>::PVOID>(tls_entry_ptr);
                        c.emu.write_memory<EmulatorTraits<Emu32>::PVOID>(tls_entry_ptr, static_cast<uint32_t>(entry.NewTlsData));
                    }
                    else
                    {
                        old_entry = c.emu.read_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr);
                        c.emu.write_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr, entry.NewTlsData);
                    }

                    entry.OldTlsData = old_entry;
                }

                entry.Flags = 2;
                data.write(entry, i++);
            }

            return STATUS_SUCCESS;
        }

        if (info_class == ProcessInstrumentationCallback)
        {
            if (process_information_length != sizeof(PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION))
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION info;

            c.emu.read_memory(process_information, &info, sizeof(PROCESS_INSTRUMENTATION_CALLBACK_INFORMATION));
            c.win_emu.callbacks.on_suspicious_activity("Setting ProcessInstrumentationCallback");

            c.proc.instrumentation_callback = info.Callback;

            return STATUS_SUCCESS;
        }

        c.win_emu.log.error("Unsupported info process class: %X\n", info_class);
        c.emu.stop();

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenProcess()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtOpenProcessToken(const syscall_context&, const handle process_handle, const ACCESS_MASK /*desired_access*/,
                                       const emulator_object<handle> token_handle)
    {
        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_NOT_SUPPORTED;
        }

        token_handle.write(CURRENT_PROCESS_TOKEN);

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtOpenProcessTokenEx(const syscall_context& c, const handle process_handle, const ACCESS_MASK desired_access,
                                         const ULONG /*handle_attributes*/, const emulator_object<handle> token_handle)
    {
        return handle_NtOpenProcessToken(c, process_handle, desired_access, token_handle);
    }

    NTSTATUS handle_NtTerminateProcess(const syscall_context& c, const handle process_handle, NTSTATUS exit_status)
    {
        if (process_handle == 0)
        {
            for (auto& thread : c.proc.threads | std::views::values)
            {
                if (&thread != c.proc.active_thread)
                {
                    thread.exit_status = exit_status;
                }
            }

            return STATUS_SUCCESS;
        }

        if (process_handle == CURRENT_PROCESS)
        {
            c.proc.exit_status = exit_status;
            c.emu.stop();
            return STATUS_SUCCESS;
        }

        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtFlushInstructionCache(const syscall_context& c, const handle process_handle,
                                            const emulator_object<uint64_t> base_address, const uint64_t region_size)
    {
        (void)c;
        (void)process_handle;
        (void)base_address;
        (void)region_size;
        return STATUS_SUCCESS;
    }
}
