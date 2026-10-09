#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

#include <utils/finally.hpp>

namespace sogen
{

    namespace syscalls
    {
        namespace
        {
            constexpr ULONG windows_cui_subsystem = 3;
            constexpr USHORT executable_large_address_aware_image = 0x0022;

            std::vector<std::u16string> parse_command_line(const std::u16string_view command_line)
            {
                std::vector<std::u16string> arguments{};
                size_t offset = 0;

                while (offset < command_line.size())
                {
                    while (offset < command_line.size() && (command_line[offset] == u' ' || command_line[offset] == u'\t'))
                    {
                        ++offset;
                    }

                    if (offset == command_line.size())
                    {
                        break;
                    }

                    std::u16string argument{};
                    bool quoted = false;
                    while (offset < command_line.size() && (quoted || (command_line[offset] != u' ' && command_line[offset] != u'\t')))
                    {
                        size_t slash_count = 0;
                        while (offset < command_line.size() && command_line[offset] == u'\\')
                        {
                            ++slash_count;
                            ++offset;
                        }

                        if (offset < command_line.size() && command_line[offset] == u'"')
                        {
                            argument.append(slash_count / 2, u'\\');
                            if ((slash_count % 2) != 0)
                            {
                                argument.push_back(u'"');
                            }
                            else
                            {
                                quoted = !quoted;
                            }
                            ++offset;
                            continue;
                        }

                        argument.append(slash_count, u'\\');
                        if (offset < command_line.size() && (quoted || (command_line[offset] != u' ' && command_line[offset] != u'\t')))
                        {
                            argument.push_back(command_line[offset++]);
                        }
                    }

                    arguments.push_back(std::move(argument));
                }

                return arguments;
            }

            std::unordered_map<std::string, std::string> read_environment(const syscall_context& c,
                                                                          const RTL_USER_PROCESS_PARAMETERS64& parameters)
            {
                std::unordered_map<std::string, std::string> environment{};
                if (parameters.Environment == 0 || parameters.EnvironmentSize == 0 || (parameters.EnvironmentSize % sizeof(char16_t)) != 0)
                {
                    return environment;
                }

                constexpr uint64_t maximum_environment_size = 16ULL << 20;
                if (parameters.EnvironmentSize > maximum_environment_size)
                {
                    throw std::runtime_error("Process environment is too large");
                }

                const auto environment_size = static_cast<size_t>(parameters.EnvironmentSize);
                std::u16string block(environment_size / sizeof(char16_t), u'\0');
                c.emu.read_memory(parameters.Environment, block.data(), environment_size);

                for (size_t offset = 0; offset < block.size() && block[offset] != u'\0';)
                {
                    const auto end = block.find(u'\0', offset);
                    if (end == std::u16string::npos)
                    {
                        break;
                    }

                    const std::u16string_view entry{block.data() + offset, end - offset};
                    const auto separator = entry.find(u'=', entry.starts_with(u'=') ? 1 : 0);
                    if (separator != std::u16string_view::npos)
                    {
                        environment.insert_or_assign(u16_to_u8(entry.substr(0, separator)), u16_to_u8(entry.substr(separator + 1)));
                    }

                    offset = end + 1;
                }

                return environment;
            }

            NTSTATUS map_process_error(const process_error error)
            {
                switch (error)
                {
                case process_error::none:
                    return STATUS_SUCCESS;
                case process_error::invalid_process:
                    return STATUS_INVALID_CID;
                case process_error::permission_denied:
                    return STATUS_ACCESS_DENIED;
                case process_error::resource_limit:
                    return STATUS_INSUFFICIENT_RESOURCES;
                case process_error::unavailable:
                    return STATUS_NOT_SUPPORTED;
                case process_error::communication_failure:
                case process_error::internal_failure:
                    return STATUS_INTERNAL_ERROR;
                }

                return STATUS_INTERNAL_ERROR;
            }

            NTSTATUS process_exit_code(const process_exit& exit)
            {
                return exit.kind == process_exit_kind::runtime_failure ? STATUS_UNSUCCESSFUL : static_cast<NTSTATUS>(exit.code);
            }
        }

        NTSTATUS handle_NtCreateUserProcess(const syscall_context& c, const emulator_object<handle> process_handle,
                                            const emulator_object<handle> thread_handle, const ACCESS_MASK /*process_desired_access*/,
                                            const ACCESS_MASK /*thread_desired_access*/,
                                            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*process_object_attributes*/,
                                            const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*thread_object_attributes*/,
                                            const ULONG /*process_flags*/, const ULONG /*thread_flags*/,
                                            const emulator_object<RTL_USER_PROCESS_PARAMETERS64> process_parameters,
                                            const emulator_object<PS_CREATE_INFO<EmulatorTraits<Emu64>>> create_info,
                                            const emulator_object<PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>> attribute_list)
        {
            auto* manager = c.win_emu.processes();
            if (!manager)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (!process_handle || !thread_handle || !process_parameters || !create_info)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto parameters = process_parameters.read();
            auto creation = create_info.read();
            if (creation.Size < sizeof(creation))
            {
                return STATUS_INVALID_PARAMETER;
            }

            size_t attribute_count = 0;
            if (attribute_list)
            {
                constexpr auto entry_size = sizeof(PS_ATTRIBUTE<EmulatorTraits<Emu64>>);
                constexpr auto header_size = sizeof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>) - entry_size;
                const auto total_length = attribute_list.read().TotalLength;
                if (total_length < header_size || (total_length - header_size) % entry_size != 0)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                attribute_count = static_cast<size_t>((total_length - header_size) / entry_size);
            }

            auto command_line = parse_command_line(read_unicode_string(c.emu, parameters.CommandLine));

            process_create_request request{};
            const windows_path application_path{read_unicode_string(c.emu, parameters.ImagePathName)};
            request.application = u16_to_u8(application_path.u16string());
            request.working_directory =
                u16_to_u8(windows_path(read_unicode_string(c.emu, parameters.CurrentDirectory.DosPath)).u16string());
            request.environment = read_environment(c, parameters);

            if (!command_line.empty())
            {
                request.argument0 = u16_to_u8(command_line.front());
                command_line.erase(command_line.begin());
            }
            request.arguments.reserve(command_line.size());
            for (const auto& argument : command_line)
            {
                request.arguments.push_back(u16_to_u8(argument));
            }

            if (request.application.empty())
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto image_arch = winpe::get_pe_arch(c.win_emu.file_sys.translate(application_path));
            const auto* pe_arch = std::get_if<winpe::pe_arch>(&image_arch);
            const auto child_is_wow64 = pe_arch && *pe_arch == winpe::pe_arch::pe32;

            const auto result = manager->create_process(std::move(request));
            if (!result)
            {
                return map_process_error(result.error);
            }
            const auto process_id = result.process_id;
            const auto thread_id = result.thread_id;

            emulator_process child{};
            child.process = result.process;
            child.id = process_id;
            child.is_wow64_process = child_is_wow64;
            child.native_environment = result.native_environment;
            child.compatibility_environment = result.compatibility_environment;
            const auto child_process_handle = c.proc.processes.store(std::move(child));

            managed_process_thread initial_thread{};
            initial_thread.process = result.process;
            initial_thread.process_id = process_id;
            initial_thread.thread_id = thread_id;
            const auto child_thread_handle = c.proc.managed_threads.store(std::move(initial_thread));

            process_handle.write(child_process_handle);
            thread_handle.write(child_thread_handle);

            creation.State = PsCreateSuccess;
            creation.SuccessState = {};
            creation.SuccessState.CurrentParameterFlags = parameters.Flags;
            creation.SuccessState.PebAddressNative = result.native_environment;
            creation.SuccessState.PebAddressWow64 = static_cast<uint32_t>(result.compatibility_environment);
            creation.SuccessState.UserProcessParametersNative = result.native_parameters;
            creation.SuccessState.UserProcessParametersWow64 = static_cast<uint32_t>(result.compatibility_parameters);
            create_info.write(creation);

            if (attribute_list)
            {
                const emulator_object<PS_ATTRIBUTE<EmulatorTraits<Emu64>>> attributes{
                    c.emu, attribute_list.value() + offsetof(PS_ATTRIBUTE_LIST<EmulatorTraits<Emu64>>, Attributes)};
                for (size_t i = 0; i < attribute_count; ++i)
                {
                    const auto attribute = attributes.read(i);
                    const auto type = attribute.Attribute & PS_ATTRIBUTE_NUMBER_MASK;
                    if (type == PsAttributeClientId && attribute.ValuePtr && attribute.Size >= sizeof(CLIENT_ID64))
                    {
                        c.emu.write_memory<CLIENT_ID64>(attribute.ValuePtr, {.UniqueProcess = process_id, .UniqueThread = thread_id});
                        if (attribute.ReturnLength)
                        {
                            c.emu.write_memory<EmulatorTraits<Emu64>::SIZE_T>(attribute.ReturnLength, sizeof(CLIENT_ID64));
                        }
                    }
                    else if (type == PsAttributeImageInfo && attribute.ValuePtr &&
                             attribute.Size >= sizeof(SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>>))
                    {
                        SECTION_IMAGE_INFORMATION<EmulatorTraits<Emu64>> image_info{};
                        image_info.MaximumStackSize = 1ULL << 20;
                        image_info.CommittedStackSize = 0x1000;
                        image_info.SubSystemType = windows_cui_subsystem;
                        image_info.SubSystemMajorVersion = 6;
                        image_info.MajorOperatingSystemVersion = 6;
                        image_info.ImageCharacteristics = executable_large_address_aware_image;
                        image_info.Machine = child_is_wow64 ? PEMachineType::I386 : PEMachineType::AMD64;
                        image_info.ImageContainsCode = TRUE;
                        c.emu.write_memory(attribute.ValuePtr, &image_info, sizeof(image_info));
                        if (attribute.ReturnLength)
                        {
                            c.emu.write_memory<EmulatorTraits<Emu64>::SIZE_T>(attribute.ReturnLength, sizeof(image_info));
                        }
                    }
                }
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtQueryInformationProcess(const syscall_context& c, const handle process_handle, const uint32_t info_class,
                                                  const uint64_t process_information, const uint32_t process_information_length,
                                                  const emulator_object<uint32_t> return_length)
        {
            if (!c.proc.is_current_process_handle(process_handle))
            {
                const auto* managed = c.proc.processes.get(process_handle);
                if (managed && managed->process)
                {
                    if (!c.win_emu.processes())
                    {
                        return STATUS_NOT_SUPPORTED;
                    }

                    if (info_class == ProcessWow64Information)
                    {
                        return handle_query<EmulatorTraits<Emu64>::ULONG_PTR>(
                            c.emu, process_information, process_information_length, return_length,
                            [&](EmulatorTraits<Emu64>::ULONG_PTR& peb32) { peb32 = managed->compatibility_environment; });
                    }

                    if (info_class != ProcessBasicInformation)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }

                    const auto status = c.win_emu.processes()->exit_status(managed->process);
                    if (!status)
                    {
                        return map_process_error(status.error);
                    }

                    const auto init_process_info = [&](PROCESS_BASIC_INFORMATION64& basic_info) {
                        basic_info = {};
                        basic_info.ExitStatus = status.status ? process_exit_code(*status.status) : STATUS_PENDING;
                        basic_info.PebBaseAddress = managed->native_environment;
                        basic_info.UniqueProcessId = managed->id;
                        basic_info.InheritedFromUniqueProcessId = c.proc.process_id;
                    };

                    switch (process_information_length)
                    {
                    case sizeof(PROCESS_BASIC_INFORMATION64):
                        return handle_query<PROCESS_BASIC_INFORMATION64>(c.emu, process_information, process_information_length,
                                                                         return_length, init_process_info);
                    case sizeof(PROCESS_EXTENDED_BASIC_INFORMATION):
                        return handle_query<PROCESS_EXTENDED_BASIC_INFORMATION>(c.emu, process_information, process_information_length,
                                                                                return_length,
                                                                                [&](PROCESS_EXTENDED_BASIC_INFORMATION& ext) {
                                                                                    ext = {};
                                                                                    ext.Size = sizeof(PROCESS_EXTENDED_BASIC_INFORMATION);
                                                                                    init_process_info(ext.BasicInfo);
                                                                                });
                    default:
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }
                }

                // The synthetic Steam process: report it as alive so a guest steam_api's GetExitCodeProcess
                // liveness check succeeds. Only ProcessBasicInformation is meaningful; any other class is
                // rejected cleanly rather than falling through to the default (which stops the emulator).
                if (process_handle == STEAM_PROCESS_HANDLE)
                {
                    if (info_class != ProcessBasicInformation)
                    {
                        return STATUS_NOT_SUPPORTED;
                    }

                    const auto init_steam_info = [&](PROCESS_BASIC_INFORMATION64& basic_info) {
                        basic_info = {};
                        basic_info.ExitStatus = STATUS_PENDING; // STILL_ACTIVE
                        basic_info.UniqueProcessId = STEAM_FAKE_PROCESS_ID;
                    };

                    switch (process_information_length)
                    {
                    case sizeof(PROCESS_BASIC_INFORMATION64):
                        return handle_query<PROCESS_BASIC_INFORMATION64>(c.emu, process_information, process_information_length,
                                                                         return_length, init_steam_info);
                    case sizeof(PROCESS_EXTENDED_BASIC_INFORMATION):
                        return handle_query<PROCESS_EXTENDED_BASIC_INFORMATION>(c.emu, process_information, process_information_length,
                                                                                return_length,
                                                                                [&](PROCESS_EXTENDED_BASIC_INFORMATION& ext) {
                                                                                    ext = {};
                                                                                    ext.Size = sizeof(PROCESS_EXTENDED_BASIC_INFORMATION);
                                                                                    init_steam_info(ext.BasicInfo);
                                                                                });
                    default:
                        return STATUS_INFO_LENGTH_MISMATCH;
                    }
                }

                return STATUS_NOT_SUPPORTED;
            }

            const auto return_length_info = c.win_emu.memory.get_region_info(return_length.value());

            switch (info_class)
            {
            case ProcessExecuteFlags:
                return handle_query<ULONG>(c.emu, process_information, process_information_length, return_length, [&](ULONG& flags) {
                    constexpr ULONG dep_enabled_execute_flags = 0x0D;
                    constexpr ULONG dep_disabled_execute_flags = 0x32;
                    flags = c.win_emu.memory.is_dep_enabled() ? dep_enabled_execute_flags : dep_disabled_execute_flags;
                });
            case ProcessGroupInformation: {
                constexpr uint32_t group_count = 1;
                constexpr uint32_t required_length = group_count * sizeof(USHORT);

                if (return_length)
                {
                    return_length.write(required_length);
                }

                // ProcessGroupInformation is a variable-length USHORT array,
                // so a larger buffer is valid.
                if (process_information_length < required_length)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const emulator_object<USHORT> info{c.emu, process_information};
                info.access([](USHORT& group) {
                    group = 0; // The process belongs to processor group 0.
                });

                return STATUS_SUCCESS;
            }
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

                return handle_query<PROCESS_MITIGATION_POLICY_RAW_DATA>(c.emu, process_information, process_information_length,
                                                                        return_length,
                                                                        [policy](PROCESS_MITIGATION_POLICY_RAW_DATA& policy_data) {
                                                                            policy_data.Policy = policy;
                                                                            policy_data.Value = 0;
                                                                        });
            }
            case ProcessEnclaveInformation:
            case ProcessTelemetryIdInformation:
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
                return handle_query<ULONG>(c.emu, process_information, process_information_length, return_length,
                                           [](ULONG& res) { res = 1; });

            case ProcessDefaultHardErrorMode:
                return handle_query<ULONG>(c.emu, process_information, process_information_length, return_length,
                                           [](ULONG& res) { res = 0; });

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

            case ProcessWow64Information:
                return handle_query<EmulatorTraits<Emu64>::ULONG_PTR>(
                    c.emu, process_information, process_information_length, return_length,
                    [&](EmulatorTraits<Emu64>::ULONG_PTR& peb32) { peb32 = c.proc.peb32 ? c.proc.peb32->value() : 0; });

            case ProcessConsoleHostProcess:
                return handle_query<EmulatorTraits<Emu64>::ULONG_PTR>(
                    c.emu, process_information, process_information_length, return_length,
                    [&](EmulatorTraits<Emu64>::ULONG_PTR& process_id) { process_id = c.proc.process_id; });

            case ProcessBasicInformation: {
                const auto init_basic_info = [&](PROCESS_BASIC_INFORMATION64& basic_info) {
                    basic_info.PebBaseAddress = c.proc.peb64.value();
                    const auto processor_count =
                        c.proc.kusd.access([](const KUSER_SHARED_DATA64& kusd) { return kusd.ActiveProcessorCount; });
                    basic_info.AffinityMask = processor_count >= 64 ? ~0ull : ((1ull << processor_count) - 1);
                    basic_info.UniqueProcessId = c.proc.process_id;
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

            case ProcessQuotaLimits: {
                constexpr uint32_t quota_limits32_size = 0x20;
                constexpr uint32_t quota_limits64_size = 0x30;

                if (process_information_length != quota_limits32_size && process_information_length != quota_limits64_size)
                {
                    if (return_length)
                    {
                        return_length.write(quota_limits64_size);
                    }
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const std::vector<std::byte> zeroed(process_information_length, std::byte{0});
                c.emu.write_memory(process_information, zeroed.data(), zeroed.size());

                if (return_length)
                {
                    return_length.write(process_information_length);
                }

                return STATUS_SUCCESS;
            }

            case ProcessVmCounters: {
                constexpr uint32_t vm_counters_size = 88;
                constexpr uint32_t vm_counters_ex_size = 96;
                constexpr uint32_t vm_counters_ex2_size = 112;

                if (process_information_length != vm_counters_size && process_information_length != vm_counters_ex_size &&
                    process_information_length != vm_counters_ex2_size)
                {
                    if (return_length)
                    {
                        return_length.write(vm_counters_ex_size);
                    }
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const std::vector<std::byte> zeroed(process_information_length, std::byte{0});
                c.emu.write_memory(process_information, zeroed.data(), zeroed.size());

                if (return_length)
                {
                    return_length.write(process_information_length);
                }

                return STATUS_SUCCESS;
            }

            case ProcessImageFileName: {
                const auto image_path = c.win_emu.mod_manager.executable->module_path.to_device_path();
                const auto string_length = image_path.size() * sizeof(char16_t);
                const auto required_length = sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>) + string_length + sizeof(char16_t);

                if (return_length)
                {
                    return_length.write(static_cast<uint32_t>(required_length));
                }

                if (process_information_length < required_length)
                {
                    return STATUS_INFO_LENGTH_MISMATCH;
                }

                const auto buffer = process_information + sizeof(UNICODE_STRING<EmulatorTraits<Emu64>>);
                c.emu.write_memory(buffer, image_path.c_str(), string_length + sizeof(char16_t));
                emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>{c.emu, process_information}.write({
                    .Length = static_cast<USHORT>(string_length),
                    .MaximumLength = static_cast<USHORT>(string_length + sizeof(char16_t)),
                    .Buffer = buffer,
                });
                return STATUS_SUCCESS;
            }

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
                c.win_emu.log.error("Unsupported process info class: 0x%X\n", info_class);
                c.emu.stop();

                return STATUS_NOT_SUPPORTED;
            }
        }

        NTSTATUS handle_NtSetInformationProcess(const syscall_context& c, const handle process_handle, const uint32_t info_class,
                                                const uint64_t process_information, const uint32_t process_information_length)
        {
            if (!c.proc.is_current_process_handle(process_handle))
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
                || info_class == ProcessQuotaLimits                          //
                || info_class == ProcessPriorityClass                        //
                || info_class == ProcessAffinityMask                         //
                || info_class == ProcessTelemetryCoverage)
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
                            cur_thread.teb32->access([&new_tls_vector](TEB32& teb32) {
                                teb32.ThreadLocalStoragePointer = static_cast<uint32_t>(new_tls_vector);
                            });
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

            c.win_emu.log.error("Unsupported info process class: 0x%X\n", info_class);
            c.emu.stop();

            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtOpenProcess(const syscall_context& c, const emulator_object<handle> process_handle,
                                      const ACCESS_MASK /*desired_access*/,
                                      const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> /*object_attributes*/,
                                      const emulator_object<CLIENT_ID64> client_id)
        {
            if (!process_handle || !client_id)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto id = client_id.read();

            // The guest opening its own pid resolves to the real guest process handle.
            if (id.UniqueProcess == c.proc.process_id)
            {
                process_handle.write(GUEST_PROCESS_HANDLE);
                return STATUS_SUCCESS;
            }

            for (const auto& [index, process] : c.proc.processes)
            {
                if (process.process && process.id == id.UniqueProcess)
                {
                    const auto handle = c.proc.processes.make_handle(index);
                    if (!c.proc.processes.duplicate(handle))
                    {
                        return STATUS_INVALID_HANDLE;
                    }
                    process_handle.write(handle);
                    return STATUS_SUCCESS;
                }
            }

            // The one synthetic external process we vouch for: the Steam client. A guest steam_api reads
            // this pid from HKCU\...\Valve\Steam\ActiveProcess and opens it to confirm Steam is running.
            if (id.UniqueProcess == STEAM_FAKE_PROCESS_ID)
            {
                process_handle.write(STEAM_PROCESS_HANDLE);
                return STATUS_SUCCESS;
            }

            return STATUS_INVALID_CID;
        }

        NTSTATUS handle_NtOpenProcessToken(const syscall_context& c, const handle process_handle, const ACCESS_MASK /*desired_access*/,
                                           const emulator_object<handle> token_handle)
        {
            if (!c.proc.is_current_process_handle(process_handle))
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
                    if (&thread != c.vcpu.active_thread)
                    {
                        c.proc.terminate_thread(thread, exit_status);
                    }
                }

                return STATUS_SUCCESS;
            }

            if (c.proc.is_current_process_handle(process_handle))
            {
                c.proc.exit_status = exit_status;
                c.win_emu.stop();
                return STATUS_SUCCESS;
            }

            const auto* managed = c.proc.processes.get(process_handle);
            if (managed && managed->process && c.win_emu.processes())
            {
                return map_process_error(c.win_emu.processes()->terminate_process(managed->process, static_cast<uint32_t>(exit_status)));
            }

            return STATUS_NOT_SUPPORTED;
        }

        NTSTATUS handle_NtFlushProcessWriteBuffers(const syscall_context& /*c*/)
        {
            return STATUS_SUCCESS;
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

} // namespace sogen
