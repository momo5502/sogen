#include "std_include.hpp"
#include "linux_emulator.hpp"

#include <address_utils.hpp>

namespace
{
    // GDT constants — same as Windows emulator
    constexpr uint64_t GDT_ADDR = 0x35000;
    constexpr uint32_t GDT_LIMIT = 0x1000;
    constexpr uint32_t GDT_ENTRY_SIZE = 8;

    // GDT entry indices
    constexpr uint16_t GDT_CODE64_SEL = 0x33; // 64-bit code segment (ring 3)
    constexpr uint16_t GDT_DATA64_SEL = 0x2B; // 64-bit data segment (ring 3)

#pragma pack(push, 1)
    struct gdt_entry
    {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t base_mid;
        uint8_t access;
        uint8_t granularity;
        uint8_t base_high;
    };
#pragma pack(pop)

    static_assert(sizeof(gdt_entry) == GDT_ENTRY_SIZE);

    void write_gdt_entry(x86_64_emulator& emu, const uint64_t gdt_base, const uint16_t selector, const uint8_t access,
                         const uint8_t granularity)
    {
        const auto index = selector >> 3;
        const auto offset = gdt_base + index * GDT_ENTRY_SIZE;

        gdt_entry entry{};
        entry.limit_low = 0xFFFF;
        entry.base_low = 0;
        entry.base_mid = 0;
        entry.access = access;
        entry.granularity = granularity;
        entry.base_high = 0;

        emu.write_memory(offset, &entry, sizeof(entry));
    }

    void setup_gdt(x86_64_emulator& emu, linux_memory_manager& memory)
    {
        // Allocate GDT page
        memory.allocate_memory(GDT_ADDR, GDT_LIMIT, memory_permission::read_write);

        // Write flat 64-bit code segment at 0x33
        // Access: present(1) | DPL=3(11) | code(1) | exec(1) | readable(1) = 0xFB
        // Granularity: long mode(1) | 4K granularity(1) | limit high(F) = 0xAF
        write_gdt_entry(emu, GDT_ADDR, GDT_CODE64_SEL, 0xFB, 0xAF);

        // Write flat 64-bit data segment at 0x2B
        // Access: present(1) | DPL=3(11) | data(0) | writable(1) = 0xF3
        // Granularity: 4K granularity(1) | 32bit(1) | limit high(F) = 0xCF
        write_gdt_entry(emu, GDT_ADDR, GDT_DATA64_SEL, 0xF3, 0xCF);

        // Load GDT
        emu.load_gdt(GDT_ADDR, GDT_LIMIT);

        // Set segment registers
        emu.reg(x86_register::cs, GDT_CODE64_SEL);
        emu.reg(x86_register::ss, GDT_DATA64_SEL);
        emu.reg(x86_register::ds, GDT_DATA64_SEL);
        emu.reg(x86_register::es, GDT_DATA64_SEL);
    }
}

linux_emulator::linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root,
                               const std::filesystem::path& executable, std::vector<std::string> argv, std::vector<std::string> envp)
    : emu_(std::move(emu)),
      emulation_root(emulation_root),
      file_sys(emulation_root),
      memory(*this->emu_),
      mod_manager(this->memory)
{
    // Set up GDT for 64-bit long mode
    setup_gdt(*this->emu_, this->memory);

    // Load the ELF binary
    this->mod_manager.map_main_modules(executable);

    if (!this->mod_manager.executable)
    {
        throw std::runtime_error("Failed to map executable: " + executable.string());
    }

    // If argv is empty, use the executable path as argv[0]
    if (argv.empty())
    {
        argv.push_back(executable.filename().string());
    }

    // Load the interpreter (dynamic linker) if required
    uint64_t interpreter_base = 0;
    uint64_t initial_rip = 0;

    if (!this->mod_manager.interpreter_path.empty())
    {
        // Resolve the interpreter path via the file system
        const auto interp_host_path = this->file_sys.translate(this->mod_manager.interpreter_path);

        if (std::filesystem::exists(interp_host_path))
        {
            this->mod_manager.interpreter = this->mod_manager.map_module(interp_host_path);

            if (this->mod_manager.interpreter)
            {
                interpreter_base = this->mod_manager.interpreter->image_base;
                initial_rip = this->mod_manager.interpreter->entry_point;
            }
        }
    }

    // Set up the synthetic vDSO (must be done before process.setup so AT_SYSINFO_EHDR is available)
    const auto vdso_base = this->vdso.setup(this->memory);

    // Bootstrap the process
    this->process.setup(*this->emu_, this->memory, *this->mod_manager.executable, argv, envp, interpreter_base, initial_rip, vdso_base);

    // Register syscall handlers
    this->dispatcher.add_handlers();

    // Install CPU hooks
    this->setup_hooks();

    this->log.info("Linux emulator initialized\n");
    this->log.info("  Executable: %s\n", executable.string().c_str());
    this->log.info("  Entry point: 0x%llx\n", static_cast<unsigned long long>(this->mod_manager.executable->entry_point));
    this->log.info("  Image base: 0x%llx\n", static_cast<unsigned long long>(this->mod_manager.executable->image_base));

    if (this->mod_manager.interpreter)
    {
        this->log.info("  Interpreter: %s (loaded at 0x%llx, entry 0x%llx)\n", this->mod_manager.interpreter_path.c_str(),
                       static_cast<unsigned long long>(this->mod_manager.interpreter->image_base),
                       static_cast<unsigned long long>(this->mod_manager.interpreter->entry_point));
    }
    else if (!this->mod_manager.interpreter_path.empty())
    {
        this->log.warn("  Interpreter: %s (not found, proceeding without it)\n", this->mod_manager.interpreter_path.c_str());
    }
}

void linux_emulator::setup_hooks()
{
    // Hook the syscall instruction
    this->emu().hook_instruction(x86_hookable_instructions::syscall, [this] {
        this->dispatcher.dispatch(*this);
        return instruction_hook_continuation::skip_instruction;
    });

    // Hook memory violations — try to deliver as SIGSEGV to user handler
    this->emu().hook_memory_violation([this](const uint64_t address, const size_t size, const memory_operation operation,
                                             const memory_violation_type type) {
        const char* type_str = (type == memory_violation_type::unmapped) ? "unmapped" : "protection";
        const char* op_str = "unknown";

        if (operation == memory_permission::read)
        {
            op_str = "read";
        }
        else if (operation == memory_permission::write)
        {
            op_str = "write";
        }
        else if (operation == memory_permission::exec)
        {
            op_str = "exec";
        }

        const auto rip = this->emu().read_instruction_pointer();

        // Determine si_code: SEGV_MAPERR for unmapped, SEGV_ACCERR for protection violation
        const int si_code = (type == memory_violation_type::unmapped) ? linux_signals::LINUX_SEGV_MAPERR : linux_signals::LINUX_SEGV_ACCERR;

        // Try to deliver SIGSEGV to the emulated program's signal handler
        if (this->signals.deliver_signal(*this, linux_signals::LINUX_SIGSEGV, address, si_code))
        {
            // Signal handler was set up — continue execution (it will run the handler)
            return memory_violation_continuation::resume;
        }

        // No handler — default action: terminate
        this->log.error("Memory violation at 0x%llx: %s %s (size=%zu, rip=0x%llx)\n", static_cast<unsigned long long>(address), type_str,
                        op_str, size, static_cast<unsigned long long>(rip));

        this->stop();
        return memory_violation_continuation::stop;
    });

    // Hook instruction execution (for counting)
    this->emu().hook_memory_execution([this](const uint64_t address) { this->on_instruction_execution(address); });
}

void linux_emulator::on_instruction_execution(const uint64_t /*address*/)
{
    ++this->executed_instructions_;

    if (this->process.active_thread)
    {
        ++this->process.active_thread->executed_instructions;
    }

    if (this->on_periodic_event && (this->executed_instructions_ % 0x20000) == 0)
    {
        this->on_periodic_event();
    }
}

void linux_emulator::start(const size_t count)
{
    this->should_stop = false;

    const auto use_count = count > 0;
    const auto start_instructions = this->executed_instructions_;
    const auto target_instructions = start_instructions + count;

    while (!this->should_stop)
    {
        this->emu().start(count);

        if (!this->emu().has_violation())
        {
            break;
        }

        if (this->should_stop)
        {
            break;
        }

        if (use_count)
        {
            const auto current_instructions = this->executed_instructions_;
            if (current_instructions >= target_instructions)
            {
                break;
            }
        }
    }
}

void linux_emulator::stop()
{
    this->should_stop = true;
    this->emu().stop();
}
