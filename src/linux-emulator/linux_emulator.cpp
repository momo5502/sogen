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

    // Allow passthrough access to the executable directory.
    // This enables loading colocated test/shared objects (e.g. RUNPATH=$ORIGIN)
    // even when the executable path itself is outside the emulation root.
    this->file_sys.add_passthrough_prefix(executable.parent_path());
    this->file_sys.add_passthrough_prefix(executable.parent_path().parent_path());

    // Provision core writable directories expected by many Linux programs.
    // With --root pointing at a minimal sysroot/project tree, /tmp often does
    // not exist yet, causing openat("/tmp/...", O_CREAT) to fail unexpectedly.
    if (!this->emulation_root.empty())
    {
        std::error_code ec{};
        std::filesystem::create_directories(this->file_sys.translate("/tmp"), ec);
        std::filesystem::create_directories(this->file_sys.translate("/var/tmp"), ec);
    }

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
        // Resolve the interpreter path via the file system.
        // Some Linux systems encode PT_INTERP as /lib64/ld-linux-x86-64.so.2,
        // while distro sysroots may place the file under /lib/x86_64-linux-gnu.
        // Try the exact PT_INTERP path first, then a few common fallbacks.
        std::vector<std::string> guest_candidates{};
        guest_candidates.push_back(this->mod_manager.interpreter_path);

        const auto interp_filename = std::filesystem::path(this->mod_manager.interpreter_path).filename().string();
        if (!interp_filename.empty())
        {
            guest_candidates.push_back("/lib/x86_64-linux-gnu/" + interp_filename);
            guest_candidates.push_back("/lib/" + interp_filename);
            guest_candidates.push_back("/usr/lib64/" + interp_filename);
            guest_candidates.push_back("/usr/lib/x86_64-linux-gnu/" + interp_filename);
        }

        std::filesystem::path interp_host_path{};
        std::string chosen_guest_path{};

        for (const auto& guest_path : guest_candidates)
        {
            const auto candidate = this->file_sys.translate(guest_path);
            if (std::filesystem::exists(candidate))
            {
                interp_host_path = candidate;
                chosen_guest_path = guest_path;
                break;
            }
        }

        if (interp_host_path.empty())
        {
            throw std::runtime_error("ELF requires dynamic linker '" + this->mod_manager.interpreter_path +
                                     "' but it was not found under --root. Tried host paths such as '" +
                                     this->file_sys.translate(this->mod_manager.interpreter_path).string() +
                                     "' and common alternatives under /lib and /usr/lib. Provide a Linux sysroot via --root containing "
                                     "this loader and required glibc/musl libraries.");
        }

        this->mod_manager.interpreter = this->mod_manager.map_module(interp_host_path, 0, false);
        if (!this->mod_manager.interpreter)
        {
            throw std::runtime_error("Failed to map dynamic linker: " + interp_host_path.string());
        }

        // Keep the selected interpreter path for logging consistency.
        this->mod_manager.interpreter_path = chosen_guest_path;

        interpreter_base = this->mod_manager.interpreter->image_base;
        initial_rip = this->mod_manager.interpreter->entry_point;
    }

    // Set up the synthetic vDSO (must be done before process.setup so AT_SYSINFO_EHDR is available)
    const auto vdso_base = this->vdso.setup(this->memory);

    // Bootstrap the process
    this->process.setup(*this->emu_, this->memory, *this->mod_manager.executable, argv, envp, interpreter_base, initial_rip, vdso_base);

    // Register syscall handlers
    this->dispatcher.add_handlers();

    // Install CPU hooks
    this->setup_hooks();

    // Resolve IRELATIVE (IFUNC) relocations eagerly only for static binaries.
    // For dynamically linked binaries, the runtime linker (ld-linux/ld-musl)
    // handles IFUNC relocation itself.
    if (!this->mod_manager.interpreter)
    {
        this->resolve_irelative_relocations();
    }

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

void linux_emulator::on_instruction_execution(const uint64_t address)
{
    ++this->executed_instructions_;

    if (this->process.active_thread)
    {
        ++this->process.active_thread->executed_instructions;
    }

    (void)address;

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

void linux_emulator::resolve_irelative_relocations()
{
    const auto& entries = this->mod_manager.irelative_entries;
    if (entries.empty())
    {
        return;
    }

    this->log.info("Resolving %zu IRELATIVE (IFUNC) relocations...\n", entries.size());

    // Save all current CPU state so we can restore it after running resolvers
    const auto saved_rip = this->emu().reg(x86_register::rip);
    const auto saved_rsp = this->emu().reg(x86_register::rsp);
    const auto saved_rax = this->emu().reg(x86_register::rax);
    const auto saved_rbx = this->emu().reg(x86_register::rbx);
    const auto saved_rcx = this->emu().reg(x86_register::rcx);
    const auto saved_rdx = this->emu().reg(x86_register::rdx);
    const auto saved_rsi = this->emu().reg(x86_register::rsi);
    const auto saved_rdi = this->emu().reg(x86_register::rdi);
    const auto saved_rbp = this->emu().reg(x86_register::rbp);
    const auto saved_r8 = this->emu().reg(x86_register::r8);
    const auto saved_r9 = this->emu().reg(x86_register::r9);
    const auto saved_r10 = this->emu().reg(x86_register::r10);
    const auto saved_r11 = this->emu().reg(x86_register::r11);
    const auto saved_r12 = this->emu().reg(x86_register::r12);
    const auto saved_r13 = this->emu().reg(x86_register::r13);
    const auto saved_r14 = this->emu().reg(x86_register::r14);
    const auto saved_r15 = this->emu().reg(x86_register::r15);
    const auto saved_rflags = this->emu().reg(x86_register::rflags);
    const auto saved_fs_base = this->emu().get_segment_base(x86_register::fs);
    const auto saved_gs_base = this->emu().get_segment_base(x86_register::gs);

    // Allocate scratch pages for the resolver execution:
    // - Sentinel page: contains a `hlt` instruction that stops the emulator when the resolver returns
    // - Stack page: a small stack for the resolver functions
    constexpr uint64_t SENTINEL_PAGE = 0x10000;       // Low address, unlikely to be used
    constexpr uint64_t RESOLVER_STACK_PAGE = 0x11000; // Stack page right after sentinel
    constexpr size_t IRELATIVE_PAGE_SIZE = 0x1000;

    const bool ok1 = this->memory.allocate_memory(SENTINEL_PAGE, IRELATIVE_PAGE_SIZE, memory_permission::all);
    const bool ok2 = this->memory.allocate_memory(RESOLVER_STACK_PAGE, IRELATIVE_PAGE_SIZE, memory_permission::read_write);

    if (!ok1 || !ok2)
    {
        this->log.warn("Failed to allocate scratch pages for IRELATIVE resolvers\n");
        if (ok1)
            this->memory.release_memory(SENTINEL_PAGE, IRELATIVE_PAGE_SIZE);
        if (ok2)
            this->memory.release_memory(RESOLVER_STACK_PAGE, IRELATIVE_PAGE_SIZE);
        return;
    }

    // Write `hlt` instruction at the sentinel address. When the resolver returns to this
    // address, `hlt` will cause a privilege fault that stops Unicorn.
    const uint8_t hlt_insn = 0xF4; // hlt
    this->memory.write_memory(SENTINEL_PAGE, &hlt_insn, 1);

    // Stack grows downward from the top of the stack page
    constexpr uint64_t RESOLVER_STACK_TOP = RESOLVER_STACK_PAGE + IRELATIVE_PAGE_SIZE;

    size_t resolved_count = 0;

    for (const auto& entry : entries)
    {
        // Set up the CPU for calling the resolver:
        // - RSP points to the resolver stack with the sentinel as return address
        // - RIP points to the resolver function
        const uint64_t resolver_sp = RESOLVER_STACK_TOP - 8; // Room for return address

        // Push sentinel address as return address
        this->memory.write_memory(resolver_sp, &SENTINEL_PAGE, sizeof(uint64_t));

        this->emu().reg(x86_register::rsp, resolver_sp);
        this->emu().reg(x86_register::rip, entry.resolver_addr);
        this->emu().reg(x86_register::rax, uint64_t{0});
        this->emu().reg(x86_register::rflags, uint64_t{0x202});

        // Run the resolver (it should execute a few instructions and `ret`).
        // When it returns to the sentinel, the `hlt` instruction will cause an
        // exception from Unicorn. We catch it and continue.
        try
        {
            this->emu().start(10000);
        }
        catch (...)
        {
            // Expected: the `hlt` instruction at the sentinel causes an exception
        }

        // Read the resolved function address from RAX
        const auto resolved_addr = this->emu().reg(x86_register::rax);

        if (resolved_addr != 0)
        {
            // Write the resolved address into the GOT slot
            this->memory.write_memory(entry.got_addr, &resolved_addr, sizeof(uint64_t));
            ++resolved_count;
        }
        else
        {
            this->log.warn("IRELATIVE resolver at 0x%llx returned 0 for GOT 0x%llx\n", static_cast<unsigned long long>(entry.resolver_addr),
                           static_cast<unsigned long long>(entry.got_addr));
        }
    }

    // Free the scratch pages
    this->memory.release_memory(SENTINEL_PAGE, IRELATIVE_PAGE_SIZE);
    this->memory.release_memory(RESOLVER_STACK_PAGE, IRELATIVE_PAGE_SIZE);

    // Restore all CPU state
    this->emu().reg(x86_register::rip, saved_rip);
    this->emu().reg(x86_register::rsp, saved_rsp);
    this->emu().reg(x86_register::rax, saved_rax);
    this->emu().reg(x86_register::rbx, saved_rbx);
    this->emu().reg(x86_register::rcx, saved_rcx);
    this->emu().reg(x86_register::rdx, saved_rdx);
    this->emu().reg(x86_register::rsi, saved_rsi);
    this->emu().reg(x86_register::rdi, saved_rdi);
    this->emu().reg(x86_register::rbp, saved_rbp);
    this->emu().reg(x86_register::r8, saved_r8);
    this->emu().reg(x86_register::r9, saved_r9);
    this->emu().reg(x86_register::r10, saved_r10);
    this->emu().reg(x86_register::r11, saved_r11);
    this->emu().reg(x86_register::r12, saved_r12);
    this->emu().reg(x86_register::r13, saved_r13);
    this->emu().reg(x86_register::r14, saved_r14);
    this->emu().reg(x86_register::r15, saved_r15);
    this->emu().reg(x86_register::rflags, saved_rflags);
    this->emu().set_segment_base(x86_register::fs, saved_fs_base);
    this->emu().set_segment_base(x86_register::gs, saved_gs_base);

    // Reset execution counters (the resolver runs shouldn't count)
    this->executed_instructions_ = 0;
    if (this->process.active_thread)
    {
        this->process.active_thread->executed_instructions = 0;
    }

    // Reset stop flag
    this->should_stop = false;

    this->log.info("Resolved %zu/%zu IRELATIVE relocations\n", resolved_count, entries.size());
}
