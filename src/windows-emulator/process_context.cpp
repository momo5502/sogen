#include "process_context.hpp"

#include "emulator_utils.hpp"
#include "windows_emulator.hpp"

namespace
{
    emulator_allocator create_allocator(memory_manager& memory, const size_t size)
    {
        const auto base = memory.find_free_allocation_base(size);
        memory.allocate_memory(base, size, memory_permission::read_write);

        return emulator_allocator{memory, base, size};
    }

    void setup_gdt(x64_emulator& emu, memory_manager& memory)
    {
        constexpr uint64_t gdtr[4] = {0, GDT_ADDR, GDT_LIMIT, 0};
        emu.write_register(x64_register::gdtr, &gdtr, sizeof(gdtr));
        memory.allocate_memory(GDT_ADDR, GDT_LIMIT, memory_permission::read);

        emu.write_memory<uint64_t>(GDT_ADDR + 6 * (sizeof(uint64_t)), 0xEFFE000000FFFF);
        emu.reg<uint16_t>(x64_register::cs, 0x33);

        emu.write_memory<uint64_t>(GDT_ADDR + 5 * (sizeof(uint64_t)), 0xEFF6000000FFFF);
        emu.reg<uint16_t>(x64_register::ss, 0x2B);
    }
}

void process_context::setup(x64_emulator& emu, memory_manager& memory, const application_settings& app_settings,
                            const emulator_settings& emu_settings, const mapped_module& executable,
                            const mapped_module& ntdll, const apiset::container& apiset_container)
{
    setup_gdt(emu, memory);

    this->kusd.setup(emu_settings.use_relative_time);

    this->base_allocator = create_allocator(memory, PEB_SEGMENT_SIZE);
    auto& allocator = this->base_allocator;

    this->peb = allocator.reserve<PEB64>();

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

    this->process_params = allocator.reserve<RTL_USER_PROCESS_PARAMETERS64>();

    this->process_params.access([&](RTL_USER_PROCESS_PARAMETERS64& proc_params) {
        proc_params.Flags = 0x6001; //| 0x80000000; // Prevent CsrClientConnectToServer

        proc_params.ConsoleHandle = CONSOLE_HANDLE.h;
        proc_params.StandardOutput = STDOUT_HANDLE.h;
        proc_params.StandardInput = STDIN_HANDLE.h;
        proc_params.StandardError = proc_params.StandardOutput;

        proc_params.Environment = reinterpret_cast<std::uint64_t*>(allocator.copy_string(u"=::=::\\"));
        allocator.copy_string(u"EMULATOR=1");
        allocator.copy_string(u"COMPUTERNAME=momo");
        allocator.copy_string(u"SystemRoot=C:\\WINDOWS");
        allocator.copy_string(u"");

        const auto application_str = app_settings.application.u16string();

        std::u16string command_line = u"\"" + application_str + u"\"";

        for (const auto& arg : app_settings.arguments)
        {
            command_line.push_back(u' ');
            command_line.append(arg);
        }

        allocator.make_unicode_string(proc_params.CommandLine, command_line);
        allocator.make_unicode_string(proc_params.CurrentDirectory.DosPath,
                                      app_settings.working_directory.u16string() + u"\\", 1024);
        allocator.make_unicode_string(proc_params.ImagePathName, application_str);

        const auto total_length = allocator.get_next_address() - this->process_params.value();

        proc_params.Length = static_cast<uint32_t>(std::max(static_cast<uint64_t>(sizeof(proc_params)), total_length));
        proc_params.MaximumLength = proc_params.Length;
    });

    this->peb.access([&](PEB64& p) {
        p.ImageBaseAddress = executable.image_base;
        p.ProcessParameters = this->process_params.ptr();
        p.ApiSetMap = apiset::clone(emu, allocator, apiset_container).ptr();

        p.ProcessHeap = nullptr;
        p.ProcessHeaps = nullptr;
        p.HeapSegmentReserve = 0x0000000000100000; // TODO: Read from executable
        p.HeapSegmentCommit = 0x0000000000002000;
        p.HeapDeCommitTotalFreeThreshold = 0x0000000000010000;
        p.HeapDeCommitFreeBlockThreshold = 0x0000000000001000;
        p.NumberOfHeaps = 0x00000000;
        p.MaximumNumberOfHeaps = 0x00000010;

        p.OSPlatformId = 2;
        p.OSMajorVersion = 0x0000000a;
        p.OSBuildNumber = 0x00006c51;

        // p.AnsiCodePageData = allocator.reserve<CPTABLEINFO>().value();
        // p.OemCodePageData = allocator.reserve<CPTABLEINFO>().value();
        p.UnicodeCaseTableData = allocator.reserve<NLSTABLEINFO>().value();
    });

    this->ntdll_image_base = ntdll.image_base;
    this->ldr_initialize_thunk = ntdll.find_export("LdrInitializeThunk");
    this->rtl_user_thread_start = ntdll.find_export("RtlUserThreadStart");
    this->ki_user_exception_dispatcher = ntdll.find_export("KiUserExceptionDispatcher");

    this->default_register_set = emu.save_registers();
}

void process_context::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->executed_instructions);
    buffer.write(this->current_ip);
    buffer.write(this->previous_ip);
    buffer.write_optional(this->exception_rip);
    buffer.write_optional(this->exit_status);
    buffer.write(this->base_allocator);
    buffer.write(this->peb);
    buffer.write(this->process_params);
    buffer.write(this->kusd);

    buffer.write(this->ntdll_image_base);
    buffer.write(this->ldr_initialize_thunk);
    buffer.write(this->rtl_user_thread_start);
    buffer.write(this->ki_user_exception_dispatcher);

    buffer.write(this->events);
    buffer.write(this->files);
    buffer.write(this->sections);
    buffer.write(this->devices);
    buffer.write(this->semaphores);
    buffer.write(this->ports);
    buffer.write(this->mutants);
    buffer.write(this->registry_keys);
    buffer.write_map(this->atoms);

    buffer.write_vector(this->default_register_set);
    buffer.write(this->spawned_thread_count);
    buffer.write(this->threads);

    buffer.write(this->threads.find_handle(this->active_thread).bits);
}

void process_context::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(this->executed_instructions);
    buffer.read(this->current_ip);
    buffer.read(this->previous_ip);
    buffer.read_optional(this->exception_rip);
    buffer.read_optional(this->exit_status);
    buffer.read(this->base_allocator);
    buffer.read(this->peb);
    buffer.read(this->process_params);
    buffer.read(this->kusd);

    buffer.read(this->ntdll_image_base);
    buffer.read(this->ldr_initialize_thunk);
    buffer.read(this->rtl_user_thread_start);
    buffer.read(this->ki_user_exception_dispatcher);

    buffer.read(this->events);
    buffer.read(this->files);
    buffer.read(this->sections);
    buffer.read(this->devices);
    buffer.read(this->semaphores);
    buffer.read(this->ports);
    buffer.read(this->mutants);
    buffer.read(this->registry_keys);
    buffer.read_map(this->atoms);

    buffer.read_vector(this->default_register_set);
    buffer.read(this->spawned_thread_count);

    for (auto& thread : this->threads | std::views::values)
    {
        thread.leak_memory();
    }

    buffer.read(this->threads);

    this->active_thread = this->threads.get(buffer.read<uint64_t>());
}

handle process_context::create_thread(memory_manager& memory, const uint64_t start_address, const uint64_t argument,
                                      const uint64_t stack_size)
{
    emulator_thread t{memory, *this, start_address, argument, stack_size, ++this->spawned_thread_count};
    return this->threads.store(std::move(t));
}
