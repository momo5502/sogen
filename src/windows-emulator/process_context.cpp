#include "std_include.hpp"
#include "process_context.hpp"

#include "emulator_utils.hpp"
#include "windows_emulator.hpp"

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

    utils::unordered_insensitive_u16string_map<std::u16string> get_environment_variables(registry_manager& registry)
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

        env_map[u"COMPUTERNAME"] = u"momo";
        env_map[u"USERNAME"] = u"momo";
        env_map[u"SystemDrive"] = u"C:";
        env_map[u"SystemRoot"] = u"C:\\WINDOWS";
        env_map[u"SystemTemp"] = u"C:\\Windows\\SystemTemp";
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

    uint32_t read_windows_build(registry_manager& registry)
    {
        const auto key = registry.get_key({R"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion)"});

        if (!key)
        {
            return 0;
        }

        for (size_t i = 0; const auto value = registry.get_value(*key, i); ++i)
        {
            if (value->type != REG_SZ)
            {
                continue;
            }

            if (value->name == "CurrentBuildNumber" || value->name == "CurrentBuild")
            {
                const auto* s = reinterpret_cast<const char16_t*>(value->data.data());
                return static_cast<uint32_t>(std::strtoul(u16_to_u8(s).c_str(), nullptr, 10));
            }
        }

        return 0;
    }
}

void process_context::setup(x86_64_emulator& emu, memory_manager& memory, registry_manager& registry,
                            const application_settings& app_settings, const mapped_module& executable, const mapped_module& ntdll,
                            const apiset::container& apiset_container, const mapped_module* ntdll32)
{
    this->windows_build_number = read_windows_build(registry);

    setup_gdt(emu, memory);

    this->kusd.setup();

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

        const auto env_map = get_environment_variables(registry);
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
        p.OSMajorVersion = 0x0000000a;
        p.OSBuildNumber = 0x00006c51;

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
            p32.OSMajorVersion = 0x0a;
            p32.OSBuildNumber = 0x6c51;

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

    if (this->gdi_shared_table_address == 0)
    {
        const auto shared_memory = allocator.reserve<GDI_SHARED_MEMORY64>();
        shared_memory.access([](GDI_SHARED_MEMORY64& mem) {
            mem = {};
            mem.Objects[0x12] = 1;
            mem.Objects[0x13] = 1;
        });
        this->gdi_shared_table_address = shared_memory.value();
    }

    this->peb64.access([&](PEB64& p) { p.GdiSharedHandleTable = this->gdi_shared_table_address; });
    if (this->is_wow64_process && this->peb32)
    {
        this->peb32->access([&](PEB32& p32) { p32.GdiSharedHandleTable = static_cast<uint32_t>(this->gdi_shared_table_address); });
    }

    if (this->is_wow64_process)
    {
        if (this->gdi_cookie == 0)
        {
            // TODO: for now let's hardcode the cookie from the DLL for determinism
            this->gdi_cookie = 0x06f2b3d5u;
        }

        this->peb64.access([&](PEB64& p) { p.GdiDCAttributeList = this->gdi_cookie; });
        if (this->peb32)
        {
            this->peb32->access([&](PEB32& p32) { p32.GdiDCAttributeList = this->gdi_cookie; });
        }
    }

    this->ntdll_image_base = ntdll.image_base;
    this->ldr_initialize_thunk = ntdll.find_export("LdrInitializeThunk");
    this->rtl_user_thread_start = ntdll.find_export("RtlUserThreadStart");
    this->ki_user_apc_dispatcher = ntdll.find_export("KiUserApcDispatcher");
    this->ki_user_exception_dispatcher = ntdll.find_export("KiUserExceptionDispatcher");
    this->instrumentation_callback = 0;

    this->default_register_set = emu.save_registers();

    this->user_handles.setup(is_wow64_process);

    auto [h, monitor_obj] = this->user_handles.allocate_object<USER_MONITOR>(handle_types::monitor);
    this->default_monitor_handle = h;
    monitor_obj.access([&](USER_MONITOR& monitor) {
        monitor.hmon = h.bits;
        monitor.rcMonitor = {.left = 0, .top = 0, .right = 1920, .bottom = 1080};
        monitor.rcWork = monitor.rcMonitor;
        if (this->is_older_windows_build())
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

void process_context::setup_callback_hook(windows_emulator& win_emu, memory_manager& memory)
{
    uint64_t sentinel_addr = this->callback_sentinel_addr;
    if (!sentinel_addr)
    {
        using sentinel_type = std::array<uint8_t, 2>;
        constexpr sentinel_type sentinel_opcodes{0x90, 0xC3}; // NOP, RET

        auto sentinel_obj = this->base_allocator.reserve_page_aligned<sentinel_type>();
        sentinel_addr = sentinel_obj.value();
        this->callback_sentinel_addr = sentinel_addr;

        win_emu.emu().write_memory(sentinel_addr, sentinel_opcodes.data(), sentinel_opcodes.size());

        const auto sentinel_aligned_length = page_align_up(sentinel_addr + sentinel_opcodes.size()) - sentinel_addr;
        memory.protect_memory(sentinel_addr, static_cast<size_t>(sentinel_aligned_length), memory_permission::all);
    }

    auto& emu = win_emu.emu();

    emu.hook_memory_execution(sentinel_addr, [&](uint64_t) {
        auto* t = this->active_thread;

        if (!t || t->callback_stack.empty())
        {
            return;
        }

        const auto frame = t->callback_stack.back();
        t->callback_stack.pop_back();

        const auto callbacks_before = t->callback_stack.size();
        const uint64_t guest_result = emu.reg(x86_register::rax);

        emu.reg(x86_register::rip, frame.rip);
        emu.reg(x86_register::rsp, frame.rsp);
        emu.reg(x86_register::r10, frame.r10);
        emu.reg(x86_register::rcx, frame.rcx);
        emu.reg(x86_register::rdx, frame.rdx);
        emu.reg(x86_register::r8, frame.r8);
        emu.reg(x86_register::r9, frame.r9);

        win_emu.dispatcher.dispatch_completion(win_emu, frame.handler_id, guest_result);

        uint64_t target_rip = emu.reg(x86_register::rip);
        emu.reg(x86_register::rip, this->callback_sentinel_addr + 1);

        const bool new_callback_dispatched = t->callback_stack.size() > callbacks_before;
        if (!new_callback_dispatched)
        {
            // Move past the syscall instruction
            target_rip += 2;
        }

        const uint64_t ret_stack_ptr = emu.reg(x86_register::rsp) - sizeof(emulator_pointer);
        emu.write_memory(ret_stack_ptr, &target_rip, sizeof(target_rip));
        emu.reg(x86_register::rsp, ret_stack_ptr);
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
    buffer.write(this->windows_build_number);
    buffer.write(this->ntdll_image_base);
    buffer.write(this->ldr_initialize_thunk);
    buffer.write(this->rtl_user_thread_start);
    buffer.write_optional(this->rtl_user_thread_start32);
    buffer.write(this->ki_user_apc_dispatcher);
    buffer.write(this->ki_user_exception_dispatcher);
    buffer.write(this->instrumentation_callback);
    buffer.write(this->gdi_shared_table_address);
    buffer.write(this->gdi_cookie);

    buffer.write(this->user_handles);
    buffer.write(this->default_monitor_handle);
    buffer.write(this->events);
    buffer.write(this->files);
    buffer.write(this->sections);
    buffer.write(this->devices);
    buffer.write(this->semaphores);
    buffer.write(this->ports);
    buffer.write(this->mutants);
    buffer.write(this->windows);
    buffer.write(this->timers);
    buffer.write(this->registry_keys);
    buffer.write_map(this->atoms);

    buffer.write(this->last_extended_params_numa_node);
    buffer.write(this->last_extended_params_attributes);
    buffer.write(this->last_extended_params_image_machine);

    buffer.write_vector(this->default_register_set);
    buffer.write(this->spawned_thread_count);
    buffer.write(this->threads);

    buffer.write(this->threads.find_handle(this->active_thread).bits);

    buffer.write(this->callback_sentinel_addr);
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
    buffer.read(this->windows_build_number);
    buffer.read(this->ntdll_image_base);
    buffer.read(this->ldr_initialize_thunk);
    buffer.read(this->rtl_user_thread_start);
    buffer.read_optional(this->rtl_user_thread_start32);
    buffer.read(this->ki_user_apc_dispatcher);
    buffer.read(this->ki_user_exception_dispatcher);
    buffer.read(this->instrumentation_callback);
    buffer.read(this->gdi_shared_table_address);
    buffer.read(this->gdi_cookie);

    buffer.read(this->user_handles);
    buffer.read(this->default_monitor_handle);
    buffer.read(this->events);
    buffer.read(this->files);
    buffer.read(this->sections);
    buffer.read(this->devices);
    buffer.read(this->semaphores);
    buffer.read(this->ports);
    buffer.read(this->mutants);
    buffer.read(this->windows);
    buffer.read(this->timers);
    buffer.read(this->registry_keys);
    buffer.read_map(this->atoms);

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

    buffer.read(this->callback_sentinel_addr);
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
    case handle_types::registry:
        return &registry_keys;
    case handle_types::mutant:
        return &mutants;
    case handle_types::port:
        return &ports;
    case handle_types::section:
        return &sections;
    default:
        return nullptr;
    }
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
