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

        switch (info_class)
        {
        case ProcessGroupInformation:
        case ProcessMitigationPolicy:
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
        case ProcessDeviceMap:
            return handle_query<EmulatorTraits<Emu64>::PVOID>(c.emu, process_information, process_information_length, return_length,
                                                              [](EmulatorTraits<Emu64>::PVOID& ptr) {
                                                                  ptr = 0; //
                                                              });

        case ProcessEnableAlignmentFaultFixup:
            return handle_query<BOOLEAN>(c.emu, process_information, process_information_length, return_length, [](BOOLEAN& b) {
                b = FALSE; //
            });

        case ProcessBasicInformation:
            return handle_query<PROCESS_BASIC_INFORMATION64>(c.emu, process_information, process_information_length, return_length,
                                                             [&](PROCESS_BASIC_INFORMATION64& basic_info) {
                                                                 basic_info.PebBaseAddress = c.proc.peb.value();
                                                                 basic_info.UniqueProcessId = 1;
                                                             });

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
            const auto peb = c.proc.peb.read();
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
            || info_class == ProcessPriorityBoost)
        {
            return STATUS_SUCCESS;
        }

        if (info_class == ProcessTlsInformation)
        {
            constexpr auto thread_data_offset = offsetof(PROCESS_TLS_INFO, ThreadData);
            if (process_information_length < thread_data_offset)
            {
                return STATUS_BUFFER_OVERFLOW;
            }

            const emulator_object<THREAD_TLS_INFO> data{c.emu, process_information + thread_data_offset};

            PROCESS_TLS_INFO tls_info{};
            c.emu.read_memory(process_information, &tls_info, thread_data_offset);

            for (uint32_t i = 0; i < tls_info.ThreadDataCount; ++i)
            {
                auto entry = data.read(i);

                const auto _ = utils::finally([&] { data.write(entry, i); });

                if (i >= c.proc.threads.size())
                {
                    entry.Flags = 0;
                    continue;
                }

                auto thread_iterator = c.proc.threads.begin();
                std::advance(thread_iterator, i);

                entry.Flags = 2;

                thread_iterator->second.teb->access([&](TEB64& teb) {
                    entry.ThreadId = teb.ClientId.UniqueThread;

                    const auto tls_vector = teb.ThreadLocalStoragePointer;
                    constexpr auto ptr_size = sizeof(EmulatorTraits<Emu64>::PVOID);

                    if (!tls_vector)
                    {
                        return;
                    }

                    if (tls_info.TlsRequest == ProcessTlsReplaceIndex)
                    {
                        const auto tls_entry_ptr = tls_vector + (tls_info.TlsIndex * ptr_size);

                        const auto old_entry = c.emu.read_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr);
                        c.emu.write_memory<EmulatorTraits<Emu64>::PVOID>(tls_entry_ptr, entry.TlsModulePointer);

                        entry.TlsModulePointer = old_entry;
                    }
                    else if (tls_info.TlsRequest == ProcessTlsReplaceVector)
                    {
                        const auto new_tls_vector = entry.TlsVector;

                        for (uint32_t index = 0; index < tls_info.TlsVectorLength; ++index)
                        {
                            const auto old_entry = c.emu.read_memory<uint64_t>(tls_vector + index * ptr_size);
                            c.emu.write_memory(new_tls_vector + index * ptr_size, old_entry);
                        }

                        teb.ThreadLocalStoragePointer = new_tls_vector;
                        entry.TlsVector = tls_vector;
                    }
                });
            }

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
}
