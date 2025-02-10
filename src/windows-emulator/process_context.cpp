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
                            const emulator_settings& emu_settings, const uint64_t process_image_base,
                            const apiset::container& apiset_container)
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

    this->peb.access([&](PEB64& peb) {
        peb.ImageBaseAddress = process_image_base;
        peb.ProcessParameters = this->process_params.ptr();
        peb.ApiSetMap = apiset::clone(emu, allocator, apiset_container).ptr();

        peb.ProcessHeap = nullptr;
        peb.ProcessHeaps = nullptr;
        peb.HeapSegmentReserve = 0x0000000000100000; // TODO: Read from executable
        peb.HeapSegmentCommit = 0x0000000000002000;
        peb.HeapDeCommitTotalFreeThreshold = 0x0000000000010000;
        peb.HeapDeCommitFreeBlockThreshold = 0x0000000000001000;
        peb.NumberOfHeaps = 0x00000000;
        peb.MaximumNumberOfHeaps = 0x00000010;

        peb.OSPlatformId = 2;
        peb.OSMajorVersion = 0x0000000a;
        peb.OSBuildNumber = 0x00006c51;

        // peb.AnsiCodePageData = allocator.reserve<CPTABLEINFO>().value();
        // peb.OemCodePageData = allocator.reserve<CPTABLEINFO>().value();
        peb.UnicodeCaseTableData = allocator.reserve<NLSTABLEINFO>().value();
    });
}
