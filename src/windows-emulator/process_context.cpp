#include "std_include.hpp"
#include "process_context.hpp"

#include "emulator_utils.hpp"
#include "syscall_utils.hpp"
#include "windows_emulator.hpp"
#include "version/windows_version_manager.hpp"

#include <utils/io.hpp>
#include <utils/buffer_accessor.hpp>
#include <regex>

namespace
{
    emulator_allocator create_allocator(memory_manager& memory, const size_t size, const bool is_wow64_process)
    {
        uint64_t default_allocation_base = (is_wow64_process == true) ? DEFAULT_ALLOCATION_ADDRESS_32BIT : DEFAULT_ALLOCATION_ADDRESS_64BIT;
        uint64_t base = memory.find_free_allocation_base(size, default_allocation_base);
        bool allocated = memory.allocate_memory(base, size, memory_permission::read_write);

        if (!allocated)
        {
            throw std::runtime_error("Failed to allocate memory for process structure");
        }

        return emulator_allocator{memory, base, size};
    }

    void setup_gdt(x86_64_emulator& emu, memory_manager& memory)
    {
        // Allocate GDT with read-write permissions for segment descriptor setup
        memory.allocate_memory(GDT_ADDR, static_cast<size_t>(page_align_up(GDT_LIMIT)), memory_permission::read_write);
        emu.load_gdt(GDT_ADDR, GDT_LIMIT);

        // Index 1 (selector 0x08) - 64-bit kernel code segment (Ring 0)
        // P=1, DPL=0, S=1, Type=0xA (Code, Execute/Read), L=1 (Long mode)
        emu.write_memory<uint64_t>(GDT_ADDR + 1 * sizeof(uint64_t), 0x00AF9B000000FFFF);

        // Index 2 (selector 0x10) - 64-bit kernel data segment (Ring 0)
        // P=1, DPL=0, S=1, Type=0x2 (Data, Read/Write), L=1 (64-bit)
        emu.write_memory<uint64_t>(GDT_ADDR + 2 * sizeof(uint64_t), 0x00CF93000000FFFF);

        // Index 3 (selector 0x18) - 32-bit compatibility mode segment (Ring 0)
        // P=1, DPL=0, S=1, Type=0xA (Code, Execute/Read), DB=1, G=1
        emu.write_memory<uint64_t>(GDT_ADDR + 3 * sizeof(uint64_t), 0x00CF9B000000FFFF);

        // Index 4 (selector 0x23) - 32-bit code segment for WOW64 (Ring 3)
        // Real Windows: Code RE Ac 3 Bg Pg P Nl 00000cfb
        // P=1, DPL=3, S=1, Type=0xA (Code, Execute/Read), DB=1, G=1
        emu.write_memory<uint64_t>(GDT_ADDR + 4 * sizeof(uint64_t), 0x00CFFB000000FFFF);

        // Index 5 (selector 0x2B) - Data segment for user mode (Ring 3)
        // Real Windows: Data RW Ac 3 Bg Pg P Nl 00000cf3
        // P=1, DPL=3, S=1, Type=0x2 (Data, Read/Write), G=1
        emu.write_memory<uint64_t>(GDT_ADDR + 5 * sizeof(uint64_t), 0x00CFF3000000FFFF);
        emu.reg<uint16_t>(x86_register::ss, 0x2B);
        emu.reg<uint16_t>(x86_register::ds, 0x2B);
        emu.reg<uint16_t>(x86_register::es, 0x2B);
        emu.reg<uint16_t>(x86_register::gs, 0x2B); // Initial GS value, will be overridden with proper base later

        // Index 6 (selector 0x33) - 64-bit code segment (Ring 3)
        // P=1, DPL=3, S=1, Type=0xA (Code, Execute/Read), L=1 (Long mode)
        emu.write_memory<uint64_t>(GDT_ADDR + 6 * sizeof(uint64_t), 0x00AFFB000000FFFF);
        emu.reg<uint16_t>(x86_register::cs, 0x33);

        // Index 10 (selector 0x53) - FS segment for WOW64 TEB access
        // Real Windows: Data RW Ac 3 Bg By P Nl 000004f3 (base=0x002c1000, limit=0xfff)
        // Initially set with base=0, will be updated during thread creation
        // P=1, DPL=3, S=1, Type=0x3 (Data, Read/Write, Accessed), G=0 (byte granularity), DB=1
        emu.write_memory<uint64_t>(GDT_ADDR + 10 * sizeof(uint64_t), 0x0040F3000000FFFF);
        emu.reg<uint16_t>(x86_register::fs, 0x53);
    }

    std::u16string expand_environment_string(const std::u16string& input,
                                             const utils::unordered_insensitive_u16string_map<std::u16string>& env_map)
    {
        std::u16string result;
        result.reserve(input.length());
        size_t pos = 0;

        while (pos < input.length())
        {
            size_t start = input.find(u'%', pos);
            if (start == std::u16string::npos)
            {
                result.append(input.substr(pos));
                break;
            }

            result.append(input.substr(pos, start - pos));

            size_t end = input.find(u'%', start + 1);
            if (end == std::u16string::npos)
            {
                result.append(input.substr(start));
                break;
            }

            std::u16string var_name = input.substr(start + 1, end - start - 1);

            if (var_name.empty())
            {
                result.append(u"%%");
            }
            else
            {
                auto it = env_map.find(var_name);
                result.append(it != env_map.end() ? it->second : input.substr(start, end - start + 1));
            }

            pos = end + 1;
        }
        return result;
    }

    utils::unordered_insensitive_u16string_map<std::u16string> get_environment_variables(registry_manager& registry,
                                                                                         const windows_version_manager& version)
    {
        utils::unordered_insensitive_u16string_map<std::u16string> env_map;
        std::unordered_set<std::u16string_view> keys_to_expand;

        const auto env_key = registry.get_key({R"(\Registry\Machine\System\CurrentControlSet\Control\Session Manager\Environment)"});
        if (env_key)
        {
            for (size_t i = 0; const auto value_opt = registry.get_value(*env_key, i); i++)
            {
                const auto& value = *value_opt;

                if (value.type != REG_SZ && value.type != REG_EXPAND_SZ)
                {
                    continue;
                }

                if (value.data.empty() || value.data.size() % 2 != 0)
                {
                    continue;
                }

                const auto char_count = value.data.size() / sizeof(char16_t);
                const auto* data_ptr = reinterpret_cast<const char16_t*>(value.data.data());
                if (data_ptr[char_count - 1] != u'\0')
                {
                    continue;
                }

                const auto [it, inserted] = env_map.emplace(u8_to_u16(value.name), std::u16string(data_ptr, char_count - 1));
                if (inserted && value.type == REG_EXPAND_SZ)
                {
                    keys_to_expand.insert(it->first);
                }
            }
        }

        env_map[u"EMULATOR"] = u"1";

        const auto* env = getenv("EMULATOR_ICICLE");
        if (env && (env == "1"sv || env == "true"sv))
        {
            env_map[u"EMULATOR_ICICLE"] = u"1";
        }

        const auto system_root = version.get_system_root().u16string();

        std::u16string system_drive = u"C:";
        if (system_root.size() >= 2 && system_root[1] == u':')
        {
            system_drive = system_root.substr(0, 2);
        }

        auto system_temp = system_root;
        if (!system_temp.empty() && system_temp.back() != u'\\')
        {
            system_temp.push_back(u'\\');
        }
        system_temp += u"SystemTemp";

        env_map[u"COMPUTERNAME"] = u"momo";
        env_map[u"USERNAME"] = u"momo";
        env_map[u"SystemDrive"] = system_drive;
        env_map[u"SystemRoot"] = system_root;
        env_map[u"SystemTemp"] = system_temp;
        env_map[u"TMP"] = u"C:\\Users\\momo\\AppData\\Temp";
        env_map[u"TEMP"] = u"C:\\Users\\momo\\AppData\\Temp";
        env_map[u"USERPROFILE"] = u"C:\\Users\\momo";

        for (const auto& key : keys_to_expand)
        {
            auto it = env_map.find(key);
            if (it != env_map.end())
            {
                std::u16string expanded = expand_environment_string(it->second, env_map);
                if (expanded != it->second)
                {
                    it->second = expanded;
                }
            }
        }

        return env_map;
    }
}

void process_context::setup(x86_64_emulator& emu, memory_manager& memory, registry_manager& registry, file_system& file_system,
                            windows_version_manager& version, const application_settings& app_settings, const mapped_module& executable,
                            const mapped_module& ntdll, const apiset::container& apiset_container, const mapped_module* ntdll32)
{
    setup_gdt(emu, memory);

    this->kusd.setup(version);

    this->base_allocator = create_allocator(memory, PEB_SEGMENT_SIZE, this->is_wow64_process);
    auto& allocator = this->base_allocator;

    this->peb64 = allocator.reserve_page_aligned<PEB64>();

    /* Values of the following fields must be
     * allocated relative to the process_params themselves
     * and included in the length:
     *
     * CurrentDirectory
     * DllPath
     * ImagePathName
     * CommandLine
     * WindowTitle
     * DesktopInfo
     * ShellInfo
     * RuntimeData
     * RedirectionDllName
     */

    this->process_params64 = allocator.reserve<RTL_USER_PROCESS_PARAMETERS64>();

    // Clone the API set for PEB64 and PEB32
    uint64_t apiset_map_address_32 = 0;
    [[maybe_unused]] const auto apiset_map_address = apiset::clone(emu, allocator, apiset_container).value();
    if (this->is_wow64_process)
    {
        apiset_map_address_32 = apiset::clone(emu, allocator, apiset_container).value();
    }

    this->process_params64.access([&](RTL_USER_PROCESS_PARAMETERS64& proc_params) {
        proc_params.Flags = 0x6001; //| 0x80000000; // Prevent CsrClientConnectToServer

        proc_params.ConsoleHandle = CONSOLE_HANDLE.h;
        proc_params.StandardOutput = STDOUT_HANDLE.h;
        proc_params.StandardInput = STDIN_HANDLE.h;
        proc_params.StandardError = proc_params.StandardOutput;

        proc_params.Environment = allocator.copy_string(u"=::=::\\");

        const auto env_map = get_environment_variables(registry, version);
        for (const auto& [name, value] : env_map)
        {
            std::u16string entry;
            entry += name;
            entry += u"=";
            entry += value;
            allocator.copy_string(entry);
        }

        allocator.copy_string(u"");

        const auto application_str = app_settings.application.u16string();

        std::u16string command_line = u"\"" + application_str + u"\"";

        for (const auto& arg : app_settings.arguments)
        {
            command_line.push_back(u' ');
            command_line.append(arg);
        }

        allocator.make_unicode_string(proc_params.CommandLine, command_line);
        allocator.make_unicode_string(proc_params.CurrentDirectory.DosPath, app_settings.working_directory.u16string() + u"\\", 1024);
        allocator.make_unicode_string(proc_params.ImagePathName, application_str);

        const auto total_length = allocator.get_next_address() - this->process_params64.value();

        proc_params.Length = static_cast<uint32_t>(std::max(static_cast<uint64_t>(sizeof(proc_params)), total_length));
        proc_params.MaximumLength = proc_params.Length;
    });

    this->peb64.access([&](PEB64& p) {
        p.BeingDebugged = 0;
        p.ImageBaseAddress = executable.image_base;
        p.ProcessParameters = this->process_params64.value();
        p.ApiSetMap = apiset::clone(emu, allocator, apiset_container).value();

        p.ProcessHeap = 0;
        p.ProcessHeaps = 0;
        p.HeapSegmentReserve = executable.size_of_heap_reserve;
        p.HeapSegmentCommit = executable.size_of_heap_commit;
        p.HeapDeCommitTotalFreeThreshold = 0x0000000000010000;
        p.HeapDeCommitFreeBlockThreshold = 0x0000000000001000;
        p.NumberOfHeaps = 0x00000000;
        p.MaximumNumberOfHeaps = 0x00000010;
        p.NumberOfProcessors = 4;
        p.ImageSubsystemMajorVersion = 6;

        p.OSPlatformId = 2;
        p.OSMajorVersion = version.get_major_version();
        p.OSMinorVersion = version.get_minor_version();
        p.OSBuildNumber = static_cast<USHORT>(version.get_windows_build_number());

        // p.AnsiCodePageData = allocator.reserve<CPTABLEINFO>().value();
        // p.OemCodePageData = allocator.reserve<CPTABLEINFO>().value();
        p.UnicodeCaseTableData = allocator.reserve<NLSTABLEINFO>().value();
    });

    if (this->is_wow64_process)
    {
        this->peb32 = allocator.reserve_page_aligned<PEB32>();

        // Initialize RTL_USER_PROCESS_PARAMETERS32 structure
        this->process_params32 = allocator.reserve<RTL_USER_PROCESS_PARAMETERS32>();

        this->process_params32->access([&](RTL_USER_PROCESS_PARAMETERS32& params32) {
            params32.Flags = RTL_USER_PROCESS_PARAMETERS_IMAGE_KEY_MISSING | RTL_USER_PROCESS_PARAMETERS_APP_MANIFEST_PRESENT |
                             RTL_USER_PROCESS_PARAMETERS_NORMALIZED;

            params32.ConsoleHandle = static_cast<uint32_t>(CONSOLE_HANDLE.h);
            params32.StandardOutput = static_cast<uint32_t>(STDOUT_HANDLE.h);
            params32.StandardInput = static_cast<uint32_t>(STDIN_HANDLE.h);
            params32.StandardError = params32.StandardOutput;

            this->process_params64.access([&](const RTL_USER_PROCESS_PARAMETERS64& params64) {
                // Copy strings from params64
                allocator.make_unicode_string(params32.ImagePathName, read_unicode_string(emu, params64.ImagePathName));
                allocator.make_unicode_string(params32.CommandLine, read_unicode_string(emu, params64.CommandLine));
                allocator.make_unicode_string(params32.DllPath, read_unicode_string(emu, params64.DllPath));
                allocator.make_unicode_string(params32.CurrentDirectory.DosPath,
                                              read_unicode_string(emu, params64.CurrentDirectory.DosPath));
                allocator.make_unicode_string(params32.WindowTitle, read_unicode_string(emu, params64.WindowTitle));
                allocator.make_unicode_string(params32.DesktopInfo, read_unicode_string(emu, params64.DesktopInfo));
                allocator.make_unicode_string(params32.ShellInfo, read_unicode_string(emu, params64.ShellInfo));
                allocator.make_unicode_string(params32.RuntimeData, read_unicode_string(emu, params64.RuntimeData));
                allocator.make_unicode_string(params32.RedirectionDllName, read_unicode_string(emu, params64.RedirectionDllName));

                // Copy other fields
                params32.CurrentDirectory.Handle = static_cast<uint32_t>(params64.CurrentDirectory.Handle);
                params32.ShowWindowFlags = params64.ShowWindowFlags;
                params32.ConsoleHandle = static_cast<uint32_t>(params64.ConsoleHandle);
                params32.ConsoleFlags = params64.ConsoleFlags;
                params32.StandardInput = static_cast<uint32_t>(params64.StandardInput);
                params32.StandardOutput = static_cast<uint32_t>(params64.StandardOutput);
                params32.StandardError = static_cast<uint32_t>(params64.StandardError);
                params32.StartingX = params64.StartingX;
                params32.StartingY = params64.StartingY;
                params32.CountX = params64.CountX;
                params32.CountY = params64.CountY;
                params32.CountCharsX = params64.CountCharsX;
                params32.CountCharsY = params64.CountCharsY;
                params32.FillAttribute = params64.FillAttribute;
                params32.WindowFlags = params64.WindowFlags;
                params32.DebugFlags = params64.DebugFlags;
                params32.ProcessGroupId = params64.ProcessGroupId;
                params32.LoaderThreads = params64.LoaderThreads;

                // Environment - copy the pointer value (both processes share the same environment)
                params32.Environment = static_cast<uint32_t>(params64.Environment);
                params32.EnvironmentSize = static_cast<uint32_t>(params64.EnvironmentSize);
                params32.EnvironmentVersion = static_cast<uint32_t>(params64.EnvironmentVersion);

                const auto total_length = allocator.get_next_address() - this->process_params32->value();

                params32.Length = static_cast<uint32_t>(std::max(static_cast<uint64_t>(sizeof(params32)), total_length));
                params32.MaximumLength = params32.Length;
            });
        });

        // Update PEB32 to point to the ProcessParameters32
        this->peb32->access([&](PEB32& p32) {
            p32.BeingDebugged = 0;
            p32.ImageBaseAddress = static_cast<uint32_t>(executable.image_base);
            p32.ProcessParameters = static_cast<uint32_t>(this->process_params32->value());

            // Use the dedicated 32-bit ApiSetMap for PEB32
            p32.ApiSetMap = static_cast<uint32_t>(apiset_map_address_32);

            // Copy similar settings from PEB64
            p32.ProcessHeap = 0;
            p32.ProcessHeaps = 0;
            p32.HeapSegmentReserve = static_cast<uint32_t>(executable.size_of_heap_reserve);
            p32.HeapSegmentCommit = static_cast<uint32_t>(executable.size_of_heap_commit);
            p32.HeapDeCommitTotalFreeThreshold = 0x00010000;
            p32.HeapDeCommitFreeBlockThreshold = 0x00001000;
            p32.NumberOfHeaps = 0;
            p32.MaximumNumberOfHeaps = 0x10;
            p32.NumberOfProcessors = 4;
            p32.ImageSubsystemMajorVersion = 6;

            p32.OSPlatformId = 2;
            p32.OSMajorVersion = version.get_major_version();
            p32.OSMinorVersion = version.get_minor_version();
            p32.OSBuildNumber = static_cast<USHORT>(version.get_windows_build_number());

            // Initialize NLS tables for 32-bit processes
            // These need to be in 32-bit addressable space
            p32.UnicodeCaseTableData = static_cast<uint32_t>(allocator.reserve<NLSTABLEINFO>().value());

            // TODO: Initialize other PEB32 fields as needed
        });

        if (ntdll32 != nullptr)
        {
            this->rtl_user_thread_start32 = ntdll32->find_export("RtlUserThreadStart");
        }
    }

    this->apiset = apiset::get_namespace_table(reinterpret_cast<const API_SET_NAMESPACE*>(apiset_container.data.data()));
    const auto& system_root = version.get_system_root();
    this->build_knowndlls_section_table<uint64_t>(registry, file_system, apiset, system_root, false);
    this->build_knowndlls_section_table<uint32_t>(registry, file_system, apiset, system_root, true);

    this->ntdll_image_base = ntdll.image_base;
    this->ldr_initialize_thunk = ntdll.find_export("LdrInitializeThunk");
    this->rtl_user_thread_start = ntdll.find_export("RtlUserThreadStart");
    this->ki_user_apc_dispatcher = ntdll.find_export("KiUserApcDispatcher");
    this->ki_user_exception_dispatcher = ntdll.find_export("KiUserExceptionDispatcher");
    this->instrumentation_callback = 0;
    this->wow64_ki_user_callback_dispatcher = 0;
    this->zw_callback_return = ntdll.find_export("ZwCallbackReturn");
    this->gdi_default_dc_handle = 0;
    this->etw_notification_event.reset();

    const auto gdi_shared_table = this->base_allocator.reserve<GDI_SHARED_MEMORY64>();
    gdi_shared_table.access([](GDI_SHARED_MEMORY64& table) { memset(&table, 0, sizeof(table)); });

    this->peb64.access([&](PEB64& peb64) {
        peb64.GdiSharedHandleTable = gdi_shared_table.value();
        peb64.GdiDCAttributeList = 0;
    });

    if (this->peb32)
    {
        uint32_t gdi_shared_table32 = 0;
        if (gdi_shared_table.value() <= std::numeric_limits<uint32_t>::max())
        {
            gdi_shared_table32 = static_cast<uint32_t>(gdi_shared_table.value());
        }

        this->peb32->access([&](PEB32& peb32) {
            peb32.GdiSharedHandleTable = gdi_shared_table32;
            peb32.GdiDCAttributeList = 0;
        });
    }

    this->default_register_set = emu.save_registers();

    this->user_handles.setup(is_wow64_process);

    auto [h, monitor_obj] = this->user_handles.allocate_object<USER_MONITOR>(handle_types::monitor);
    this->default_monitor_handle = h;
    monitor_obj.access([&](USER_MONITOR& monitor) {
        monitor.hmon = h.bits;
        monitor.rcMonitor = {.left = 0, .top = 0, .right = 1920, .bottom = 1080};
        monitor.rcWork = monitor.rcMonitor;
        if (version.is_build_before(26040))
        {
            monitor.b20.monitorDpi = 96;
            monitor.b20.nativeDpi = monitor.b20.monitorDpi;
            monitor.b20.cachedDpi = monitor.b20.monitorDpi;
            monitor.b20.rcMonitorDpiAware = monitor.rcMonitor;
        }
        else
        {
            monitor.b26.monitorDpi = 96;
            monitor.b26.nativeDpi = monitor.b26.monitorDpi;
        }
    });

    const auto user_display_info = this->user_handles.get_display_info();
    user_display_info.access([&](USER_DISPINFO& display_info) {
        display_info.dwMonitorCount = 1;
        display_info.pPrimaryMonitor = monitor_obj.value();
    });
}

void process_context::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->shared_section_address);
    buffer.write(this->shared_section_size);
    buffer.write(this->dbwin_buffer);
    buffer.write(this->dbwin_buffer_size);
    buffer.write_optional(this->exit_status);
    buffer.write(this->base_allocator);
    buffer.write(this->peb64);
    buffer.write_optional(this->peb32);
    buffer.write(this->process_params64);
    buffer.write_optional(this->process_params32);
    buffer.write(this->kusd);

    buffer.write(this->is_wow64_process);
    buffer.write(this->ntdll_image_base);
    buffer.write(this->ldr_initialize_thunk);
    buffer.write(this->rtl_user_thread_start);
    buffer.write_optional(this->rtl_user_thread_start32);
    buffer.write(this->ki_user_apc_dispatcher);
    buffer.write(this->ki_user_exception_dispatcher);
    buffer.write(this->instrumentation_callback);
    buffer.write(this->wow64_ki_user_callback_dispatcher);
    buffer.write(this->zw_callback_return);
    buffer.write(this->dispatch_client_message);
    buffer.write(this->gdi_default_dc_handle);
    buffer.write_optional(this->etw_notification_event);

    buffer.write(this->user_handles);
    buffer.write(this->default_monitor_handle);
    buffer.write(this->events);
    buffer.write(this->files);
    buffer.write(this->sections);
    buffer.write(this->devices);
    buffer.write(this->semaphores);
    buffer.write(this->io_completions);
    buffer.write(this->wait_completion_packets);
    buffer.write(this->worker_factories);
    buffer.write(this->ports);
    buffer.write(this->mutants);
    buffer.write(this->default_desktop);
    buffer.write(this->desktops);
    buffer.write(this->windows);
    buffer.write(this->timers);
    buffer.write(this->registry_keys);
    buffer.write_map(this->atoms);
    buffer.write_map(this->classes);

    buffer.write_map(this->apiset);
    buffer.write_map(this->knowndlls32_sections);
    buffer.write_map(this->knowndlls64_sections);

    buffer.write(this->last_extended_params_numa_node);
    buffer.write(this->last_extended_params_attributes);
    buffer.write(this->last_extended_params_image_machine);

    buffer.write_vector(this->default_register_set);
    buffer.write(this->spawned_thread_count);
    buffer.write(this->threads);

    buffer.write(this->threads.find_handle(this->active_thread).bits);
}

void process_context::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(this->shared_section_address);
    buffer.read(this->shared_section_size);
    buffer.read(this->dbwin_buffer);
    buffer.read(this->dbwin_buffer_size);
    buffer.read_optional(this->exit_status);
    buffer.read(this->base_allocator);
    buffer.read(this->peb64);
    buffer.read_optional(this->peb32);
    buffer.read(this->process_params64);
    buffer.read_optional(this->process_params32);
    buffer.read(this->kusd);

    buffer.read(this->is_wow64_process);
    buffer.read(this->ntdll_image_base);
    buffer.read(this->ldr_initialize_thunk);
    buffer.read(this->rtl_user_thread_start);
    buffer.read_optional(this->rtl_user_thread_start32);
    buffer.read(this->ki_user_apc_dispatcher);
    buffer.read(this->ki_user_exception_dispatcher);
    buffer.read(this->instrumentation_callback);
    buffer.read(this->wow64_ki_user_callback_dispatcher);
    buffer.read(this->zw_callback_return);
    buffer.read(this->dispatch_client_message);
    buffer.read(this->gdi_default_dc_handle);
    buffer.read_optional(this->etw_notification_event);

    buffer.read(this->user_handles);
    buffer.read(this->default_monitor_handle);
    buffer.read(this->events);
    buffer.read(this->files);
    buffer.read(this->sections);
    buffer.read(this->devices);
    buffer.read(this->semaphores);
    buffer.read(this->io_completions);
    buffer.read(this->wait_completion_packets);
    buffer.read(this->worker_factories);
    buffer.read(this->ports);
    buffer.read(this->mutants);
    buffer.read(this->default_desktop);
    buffer.read(this->desktops);
    buffer.read(this->windows);
    buffer.read(this->timers);
    buffer.read(this->registry_keys);
    buffer.read_map(this->atoms);
    buffer.read_map(this->classes);

    buffer.read_map(this->apiset);
    buffer.read_map(this->knowndlls32_sections);
    buffer.read_map(this->knowndlls64_sections);

    buffer.read(this->last_extended_params_numa_node);
    buffer.read(this->last_extended_params_attributes);
    buffer.read(this->last_extended_params_image_machine);

    buffer.read_vector(this->default_register_set);
    buffer.read(this->spawned_thread_count);

    for (auto& thread : this->threads | std::views::values)
    {
        thread.leak_memory();
    }

    buffer.read(this->threads);

    this->active_thread = this->threads.get(buffer.read<uint64_t>());
}

generic_handle_store* process_context::get_handle_store(const handle handle)
{
    switch (handle.value.type)
    {
    case handle_types::thread:
        return &threads;
    case handle_types::event:
        return &events;
    case handle_types::file:
        return &files;
    case handle_types::device:
        return &devices;
    case handle_types::semaphore:
        return &semaphores;
    case handle_types::io_completion:
        return &io_completions;
    case handle_types::wait_completion_packet:
        return &wait_completion_packets;
    case handle_types::worker_factory:
        return &worker_factories;
    case handle_types::registry:
        return &registry_keys;
    case handle_types::mutant:
        return &mutants;
    case handle_types::timer:
        return &timers;
    case handle_types::desktop:
        return &desktops;
    case handle_types::port:
        return &ports;
    case handle_types::section:
        return &sections;
    default:
        return nullptr;
    }
}

size_t process_context::get_live_thread_count() const
{
    return std::count_if(threads.begin(), threads.end(), [](auto& item) { return !item.second.is_terminated(); });
}

handle process_context::create_thread(memory_manager& memory, const uint64_t start_address, const uint64_t argument,
                                      const uint64_t stack_size, const uint32_t create_flags, const bool initial_thread)
{
    emulator_thread t{memory, *this, start_address, argument, stack_size, create_flags, ++this->spawned_thread_count, initial_thread};
    auto [h, thr] = this->threads.store_and_get(std::move(t));
    this->callbacks_->on_thread_create(h, *thr);
    return h;
}

std::optional<uint16_t> process_context::find_atom(const std::u16string_view name)
{
    for (auto& entry : this->atoms)
    {
        if (utils::string::equals_ignore_case(std::u16string_view{entry.second.name}, name))
        {
            ++entry.second.ref_count;
            return entry.first;
        }
    }

    return {};
}

uint16_t process_context::add_or_find_atom(std::u16string name)
{
    uint16_t index = 1;
    if (!this->atoms.empty())
    {
        auto i = this->atoms.end();
        --i;
        index = i->first + 1;
    }

    std::optional<uint16_t> last_entry{};
    for (auto& entry : this->atoms)
    {
        if (utils::string::equals_ignore_case(entry.second.name, name))
        {
            ++entry.second.ref_count;
            return entry.first;
        }

        if (entry.first > 0)
        {
            if (!last_entry)
            {
                index = 1;
            }
            else
            {
                const auto diff = entry.first - *last_entry;
                if (diff > 1)
                {
                    index = *last_entry + 1;
                }
            }
        }

        last_entry = entry.first;
    }

    atoms[index] = {std::move(name), 1};

    return index;
}

bool process_context::delete_atom(const std::u16string& name)
{
    for (auto it = atoms.begin(); it != atoms.end(); ++it)
    {
        if (utils::string::equals_ignore_case(it->second.name, name))
        {
            if (--it->second.ref_count == 0)
            {
                atoms.erase(it);
            }
            return true;
        }
    }

    return false;
}

bool process_context::delete_atom(const uint16_t atom_id)
{
    const auto it = atoms.find(atom_id);
    if (it == atoms.end())
    {
        return false;
    }

    if (--it->second.ref_count == 0)
    {
        atoms.erase(it);
    }

    return true;
}

const std::u16string* process_context::get_atom_name(const uint16_t atom_id) const
{
    const auto it = atoms.find(atom_id);
    if (it == atoms.end())
    {
        return nullptr;
    }

    return &it->second.name;
}

template <typename T>
void process_context::build_knowndlls_section_table(registry_manager& registry, const file_system& file_system, const apiset_map& apiset,
                                                    const windows_path& system_root, bool is_32bit)
{
    windows_path system_root_path;
    std::set<std::u16string> visisted;
    std::queue<std::u16string> q;

    system_root_path = is_32bit ? (system_root / "SysWOW64") : (system_root / "System32");

    std::optional<registry_key> knowndlls_key =
        registry.get_key({R"(\Registry\Machine\System\CurrentControlSet\Control\Session Manager\KnownDLLs)"});
    if (!knowndlls_key)
    {
        return;
    }

    size_t i = 0;
    for (;;)
    {
        auto known_dll_name_opt = registry.read_u16string(knowndlls_key.value(), i++);

        if (!known_dll_name_opt)
        {
            break;
        }

        auto known_dll_name = known_dll_name_opt.value();
        utils::string::to_lower_inplace(known_dll_name);

        q.push(known_dll_name);
        visisted.insert(known_dll_name);
    }

    while (!q.empty())
    {
        auto knowndll_filename = q.front();
        q.pop();

        std::vector<std::byte> file;
        if (!utils::io::read_file(file_system.translate(system_root_path / knowndll_filename), &file))
        {
            continue;
        }

        section s;
        s.file_name = (system_root_path / knowndll_filename).u16string();
        s.maximum_size = 0;
        s.allocation_attributes = SEC_IMAGE;
        s.section_page_protection = PAGE_EXECUTE;
        s.cache_image_info_from_filedata(file);
        add_knowndll_section(knowndll_filename, s, is_32bit);

        utils::safe_buffer_accessor<const std::byte> buffer{file};
        const auto dos_header = buffer.as<PEDosHeader_t>(0).get();
        const auto nt_headers_offset = dos_header.e_lfanew;
        const auto nt_headers = buffer.as<PENTHeaders_t<T>>(static_cast<size_t>(nt_headers_offset)).get();

        const auto& import_directory_entry = winpe::get_data_directory_by_index(nt_headers, IMAGE_DIRECTORY_ENTRY_IMPORT);
        if (!import_directory_entry.VirtualAddress)
        {
            continue;
        }

        const auto section_with_import_descs =
            winpe::get_section_header_by_rva(buffer, nt_headers, nt_headers_offset, import_directory_entry.VirtualAddress);
        auto import_directory_vbase = section_with_import_descs.VirtualAddress;
        auto import_directory_rbase = section_with_import_descs.PointerToRawData;

        uint64_t import_directory_raw =
            rva_to_file_offset(import_directory_vbase, import_directory_rbase, import_directory_entry.VirtualAddress);
        auto import_descriptors = buffer.as<IMAGE_IMPORT_DESCRIPTOR>(static_cast<size_t>(import_directory_raw));

        for (size_t import_desc_index = 0;; import_desc_index++)
        {
            const auto descriptor = import_descriptors.get(import_desc_index);
            if (!descriptor.Name)
            {
                break;
            }

            auto known_dll_dep_name = u8_to_u16(
                buffer.as_string(static_cast<size_t>(rva_to_file_offset(import_directory_vbase, import_directory_rbase, descriptor.Name))));
            utils::string::to_lower_inplace(known_dll_dep_name);

            if (known_dll_dep_name.starts_with(u"api-") || known_dll_dep_name.starts_with(u"ext-"))
            {
                if (auto apiset_entry = apiset.find(known_dll_dep_name); apiset_entry != apiset.end())
                {
                    known_dll_dep_name = apiset_entry->second;
                }
                else
                {
                    continue;
                }
            }

            if (!visisted.contains(known_dll_dep_name))
            {
                q.push(known_dll_dep_name);
                visisted.insert(known_dll_dep_name);
            }
        }
    }
}

bool process_context::has_knowndll_section(const std::u16string& name, bool is_32bit) const
{
    auto lname = utils::string::to_lower(name);

    if (is_32bit)
    {
        return knowndlls32_sections.contains(lname);
    }

    return knowndlls64_sections.contains(lname);
}

std::optional<section> process_context::get_knowndll_section_by_name(const std::u16string& name, bool is_32bit) const
{
    auto lname = utils::string::to_lower(name);

    if (is_32bit)
    {
        if (auto section = knowndlls32_sections.find(lname); section != knowndlls32_sections.end())
        {
            return section->second;
        }
    }
    else
    {
        if (auto section = knowndlls64_sections.find(lname); section != knowndlls64_sections.end())
        {
            return section->second;
        }
    }

    return {};
}

void process_context::add_knowndll_section(const std::u16string& name, const section& section, bool is_32bit)
{
    auto lname = utils::string::to_lower(name);

    if (is_32bit)
    {
        knowndlls32_sections[lname] = section;
    }
    else
    {
        knowndlls64_sections[lname] = section;
    }
}
