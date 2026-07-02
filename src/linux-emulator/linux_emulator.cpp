#include "std_include.hpp"
#include "linux_emulator.hpp"

#include <address_utils.hpp>

namespace sogen
{

    namespace
    {
        constexpr std::string_view LINUX_EMULATOR_STATE_VERSION = "linux-emulator-state-v1";

        // GDT constants — same as Windows emulator
        constexpr uint64_t GDT_ADDR = 0x35000;
        constexpr uint32_t GDT_LIMIT = 0x1000;
        constexpr uint32_t GDT_ENTRY_SIZE = 8;

        // GDT entry indices
        constexpr uint16_t GDT_KERNEL_CODE64_SEL = 0x08;
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

            write_gdt_entry(emu, GDT_ADDR, GDT_KERNEL_CODE64_SEL, 0x9B, 0xAF);

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

        std::string format_memory_violation_stop_detail(const uint64_t address, const size_t size)
        {
            std::array<char, 96> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "address=0x%" PRIx64 " size=%zu", address, size);
            return buffer.data();
        }

        struct instruction_fetch_violation
        {
            uint64_t address{};
            memory_violation_type type{};
        };

        std::optional<instruction_fetch_violation> classify_instruction_fetch_violation(const linux_memory_manager& memory,
                                                                                        const uint64_t address)
        {
            const auto region = memory.get_region_info(address);
            if (!region.has_value())
            {
                return instruction_fetch_violation{.address = address, .type = memory_violation_type::unmapped};
            }

            if (!is_executable(region->permissions))
            {
                return instruction_fetch_violation{.address = address, .type = memory_violation_type::protection};
            }

            return std::nullopt;
        }

        uint64_t current_time_ns()
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        }

        constexpr uint8_t X86_INT3_OPCODE = 0xCC;

        struct int3_signal_location
        {
            uint64_t fault_addr{};
            uint64_t resume_rip{};
        };

        int3_signal_location resolve_int3_signal_location(const linux_memory_manager& memory, const uint64_t reported_rip)
        {
            uint8_t opcode{};
            if (memory.try_read_memory(reported_rip, &opcode, sizeof(opcode)) && opcode == X86_INT3_OPCODE)
            {
                return {.fault_addr = reported_rip, .resume_rip = reported_rip + 1};
            }

            if (reported_rip > 0 && memory.try_read_memory(reported_rip - 1, &opcode, sizeof(opcode)) && opcode == X86_INT3_OPCODE)
            {
                return {.fault_addr = reported_rip - 1, .resume_rip = reported_rip};
            }

            return {.fault_addr = reported_rip, .resume_rip = reported_rip};
        }

        struct resolved_application_path
        {
            std::filesystem::path host_path{};
            std::string guest_path{};
        };

        std::optional<std::string> guest_path_from_root(const linux_file_system& file_sys, const std::filesystem::path& host_path)
        {
            const auto& root = file_sys.root();
            if (root.empty())
            {
                return std::nullopt;
            }

            const auto normalized_root = root.lexically_normal();
            const auto normalized_host = host_path.lexically_normal();
            const auto relative = normalized_host.lexically_relative(normalized_root);
            if (relative.empty() || relative == "." || *relative.begin() == "..")
            {
                return std::nullopt;
            }

            return linux_file_system::normalize_guest_path_string("/" + relative.generic_string());
        }

        resolved_application_path resolve_application_path(const linux_file_system& file_sys, const std::filesystem::path& executable)
        {
            std::error_code ec{};
            auto host_path = file_sys.translate(executable.string());
            if (std::filesystem::exists(host_path, ec))
            {
                auto guest_path = guest_path_from_root(file_sys, host_path);
                if (!guest_path)
                {
                    guest_path = linux_file_system::normalize_guest_path_string(executable.generic_string());
                }

                return {.host_path = std::move(host_path), .guest_path = std::move(*guest_path)};
            }

            if (executable.is_absolute() && std::filesystem::exists(executable, ec))
            {
                host_path = executable.lexically_normal();
                auto guest_path = guest_path_from_root(file_sys, host_path);
                if (!guest_path)
                {
                    guest_path = linux_file_system::normalize_guest_path_string("/" + host_path.filename().generic_string());
                }

                return {.host_path = std::move(host_path), .guest_path = std::move(*guest_path)};
            }

            auto guest_path = guest_path_from_root(file_sys, host_path);
            if (!guest_path)
            {
                guest_path = linux_file_system::normalize_guest_path_string(executable.generic_string());
            }

            return {.host_path = std::move(host_path), .guest_path = std::move(*guest_path)};
        }
    }

    linux_emulator::linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root)
        : emu_(std::move(emu)),
          emulation_root(emulation_root),
          file_sys(emulation_root),
          memory(*this->emu_),
          mod_manager(this->memory)
    {
        this->mod_manager.on_module_load.add([this](linux_mapped_module& module) { this->on_module_load(module); });
        this->initialize_cpu_and_filesystem();
    }

    linux_emulator::linux_emulator(std::unique_ptr<x86_64_emulator> emu, const std::filesystem::path& emulation_root,
                                   const std::filesystem::path& executable, std::vector<std::string> argv,
                                   const std::vector<std::string>& envp)
        : emu_(std::move(emu)),
          emulation_root(emulation_root),
          file_sys(emulation_root),
          memory(*this->emu_),
          mod_manager(this->memory)
    {
        this->mod_manager.on_module_load.add([this](linux_mapped_module& module) { this->on_module_load(module); });
        this->initialize_cpu_and_filesystem();
        this->load_application(executable, std::move(argv), envp);
    }

    void linux_emulator::initialize_cpu_and_filesystem()
    {
        // Set up GDT for 64-bit long mode
        setup_gdt(*this->emu_, this->memory);

        // Provision core writable directories expected by many Linux programs.
        // With --root pointing at a minimal sysroot/project tree, /tmp often does
        // not exist yet, causing openat("/tmp/...", O_CREAT) to fail unexpectedly.
        if (!this->emulation_root.empty())
        {
            std::error_code ec{};
            std::filesystem::create_directories(this->file_sys.translate("/tmp"), ec);
            std::filesystem::create_directories(this->file_sys.translate("/var/tmp"), ec);
        }

        // Register syscall handlers
        this->dispatcher.add_handlers();

        // Install CPU hooks
        this->setup_hooks();
    }

    void linux_emulator::load_application(const std::filesystem::path& executable, std::vector<std::string> argv,
                                          const std::vector<std::string>& envp)
    {
        const auto executable_path = resolve_application_path(this->file_sys, executable);

        // Allow passthrough access to the resolved executable directory so
        // colocated dependencies (for example RUNPATH=$ORIGIN) can be loaded
        // when the executable itself is outside the emulation root. Resolve
        // first so a guest path such as /bin/foo prefers <root>/bin/foo over
        // the host /bin/foo even when both exist.
        this->file_sys.add_passthrough_prefix(executable_path.host_path.parent_path());

        // Load the ELF binary
        this->mod_manager.map_main_modules(executable_path.host_path);

        if (!this->mod_manager.executable)
        {
            throw std::runtime_error("Failed to map executable: " + executable.string());
        }

        // If argv is empty, use the executable path as argv[0]
        if (argv.empty())
        {
            argv.push_back(executable_path.guest_path);
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
        this->process.setup(*this->emu_, this->memory, *this->mod_manager.executable, argv, envp, executable_path.guest_path,
                            interpreter_base, initial_rip, vdso_base);

        // Resolve IRELATIVE (IFUNC) relocations eagerly only for static binaries.
        // For dynamically linked binaries, the runtime linker (ld-linux/ld-musl)
        // handles IFUNC relocation itself.
        if (!this->mod_manager.interpreter)
        {
            this->resolve_irelative_relocations();
        }

        this->log.info("Linux emulator initialized\n");
        this->log.info("  Executable: %s\n", executable.string().c_str());
        this->log.info("  Entry point: 0x%" PRIx64 "\n", this->mod_manager.executable->entry_point);
        this->log.info("  Image base: 0x%" PRIx64 "\n", this->mod_manager.executable->image_base);

        if (this->mod_manager.interpreter)
        {
            this->log.info("  Interpreter: %s (loaded at 0x%" PRIx64 ", entry 0x%" PRIx64 ")\n", this->mod_manager.interpreter_path.c_str(),
                           this->mod_manager.interpreter->image_base, this->mod_manager.interpreter->entry_point);
        }
    }

    linux_thread* linux_emulator::current_thread() const
    {
        auto* thread = this->process.active_thread;
        if (!thread || thread->terminated)
        {
            return nullptr;
        }

        return thread;
    }

    std::optional<uint32_t> linux_emulator::current_thread_id() const
    {
        const auto* thread = this->current_thread();
        if (!thread)
        {
            return std::nullopt;
        }

        return thread->tid;
    }

    bool linux_emulator::activate_thread(const uint32_t tid)
    {
        auto it = this->process.threads.find(tid);
        if (it == this->process.threads.end() || it->second.terminated)
        {
            return false;
        }

        auto* old_thread = this->process.active_thread;
        if (old_thread && !old_thread->terminated)
        {
            old_thread->save(this->emu());
        }

        auto& new_thread = it->second;
        const auto old_tid = old_thread ? old_thread->tid : 0;
        this->process.active_thread = &new_thread;
        new_thread.restore(this->emu());

        if (old_tid != new_thread.tid)
        {
            this->on_thread_switch(old_tid, new_thread.tid);
        }

        return true;
    }

    bool linux_emulator::perform_thread_switch()
    {
        if (this->process.threads.empty())
        {
            return false;
        }

        const auto old_tid = this->process.active_thread ? this->process.active_thread->tid : 0;
        const auto now = current_time_ns();
        auto start = this->process.threads.upper_bound(old_tid);
        for (size_t checked = 0; checked < this->process.threads.size(); ++checked)
        {
            if (start == this->process.threads.end())
            {
                start = this->process.threads.begin();
            }

            auto& candidate = start->second;
            ++start;
            if (candidate.tid == old_tid || !candidate.is_thread_ready(now))
            {
                continue;
            }

            return this->activate_thread(candidate.tid);
        }

        return false;
    }

    void linux_emulator::yield_thread()
    {
        if (this->perform_thread_switch())
        {
            this->stop();
        }
    }

    void linux_emulator::record_stop(const stop_reason reason, std::string detail)
    {
        this->last_stop_reason_ = reason;
        this->last_stop_detail_ = std::move(detail);
    }

    void linux_emulator::serialize(utils::buffer_serializer& buffer, const bool is_snapshot) const
    {
        buffer.write(std::string{LINUX_EMULATOR_STATE_VERSION});
        this->emu().serialize_state(buffer, is_snapshot);
        buffer.write(this->executed_instructions_);
        buffer.write(this->last_stop_reason_);
        buffer.write(this->last_stop_detail_);
        this->memory.serialize_memory_state(buffer);
        this->process.serialize(buffer);
        this->mod_manager.serialize(buffer);
        this->signals.serialize(buffer);
        this->vdso.serialize(buffer);
    }

    void linux_emulator::deserialize(utils::buffer_deserializer& buffer, const bool is_snapshot)
    {
        const auto apply_state = [this, is_snapshot](utils::buffer_deserializer& state_buffer) {
            const auto version = state_buffer.read<std::string>();
            if (version != LINUX_EMULATOR_STATE_VERSION)
            {
                throw std::runtime_error("Unsupported Linux emulator state version: " + version);
            }

            this->emu().deserialize_state(state_buffer, is_snapshot);
            state_buffer.read(this->executed_instructions_);
            state_buffer.read(this->last_stop_reason_);
            state_buffer.read(this->last_stop_detail_);
            this->memory.deserialize_memory_state(state_buffer);
            this->process.deserialize(state_buffer);
            this->mod_manager.deserialize(state_buffer);
            this->signals.deserialize(state_buffer);
            this->vdso.deserialize(state_buffer);
            this->should_stop = false;
        };

        utils::buffer_serializer rollback{};
        bool have_rollback = false;
        try
        {
            this->serialize(rollback, is_snapshot);
            have_rollback = true;
        }
        catch (...)
        {
            have_rollback = false;
        }

        try
        {
            apply_state(buffer);
        }
        catch (...)
        {
            if (have_rollback)
            {
                try
                {
                    utils::buffer_deserializer rollback_buffer{rollback};
                    apply_state(rollback_buffer);
                }
                catch (...)
                {
                }
            }
            throw;
        }
    }

    uint64_t linux_emulator::add_memory_violation_observer(
        std::function<memory_violation_continuation(uint64_t, size_t, memory_operation, memory_violation_type)> observer)
    {
        const auto id = this->next_memory_violation_observer_id_++;
        this->memory_violation_observers_.emplace_back(id, std::move(observer));
        return id;
    }

    void linux_emulator::remove_memory_violation_observer(const uint64_t id)
    {
        for (auto it = this->memory_violation_observers_.begin(); it != this->memory_violation_observers_.end(); ++it)
        {
            if (it->first == id)
            {
                this->memory_violation_observers_.erase(it);
                return;
            }
        }
    }

    uint64_t linux_emulator::add_interrupt_observer(std::function<void(int)> observer)
    {
        const auto id = this->next_interrupt_observer_id_++;
        this->interrupt_observers_.emplace_back(id, std::make_shared<std::function<void(int)>>(std::move(observer)));
        return id;
    }

    void linux_emulator::remove_interrupt_observer(const uint64_t id)
    {
        for (auto it = this->interrupt_observers_.begin(); it != this->interrupt_observers_.end(); ++it)
        {
            if (it->first == id)
            {
                this->interrupt_observers_.erase(it);
                return;
            }
        }
    }

    void linux_emulator::notify_interrupt_observers(const int interrupt)
    {
        std::vector<uint64_t> observer_ids{};
        observer_ids.reserve(this->interrupt_observers_.size());
        for (const auto& observer : this->interrupt_observers_)
        {
            observer_ids.push_back(observer.first);
        }

        for (const auto observer_id : observer_ids)
        {
            const auto it = std::ranges::find_if(this->interrupt_observers_,
                                                 [observer_id](const auto& observer) { return observer.first == observer_id; });
            if (it == this->interrupt_observers_.end())
            {
                continue;
            }

            auto observer = it->second;
            (*observer)(interrupt);
            if (this->should_stop)
            {
                return;
            }
        }
    }

    memory_violation_continuation linux_emulator::notify_memory_violation_observers(const uint64_t address, const size_t size,
                                                                                    const memory_operation operation,
                                                                                    const memory_violation_type type)
    {
        auto continuation = memory_violation_continuation::resume;
        std::vector<uint64_t> observer_ids{};
        observer_ids.reserve(this->memory_violation_observers_.size());
        for (const auto& observer : this->memory_violation_observers_)
        {
            observer_ids.push_back(observer.first);
        }

        for (const auto observer_id : observer_ids)
        {
            const auto it = std::ranges::find_if(this->memory_violation_observers_,
                                                 [observer_id](const auto& observer) { return observer.first == observer_id; });
            if (it == this->memory_violation_observers_.end())
            {
                continue;
            }

            auto observer = it->second;
            const auto result = observer(address, size, operation, type);
            if (result == memory_violation_continuation::stop)
            {
                this->record_stop(stop_reason::unhandled_memory_violation, format_memory_violation_stop_detail(address, size));
                this->process.exit_status.reset();
                this->stop();
                return memory_violation_continuation::stop;
            }

            if (result == memory_violation_continuation::restart)
            {
                continuation = memory_violation_continuation::restart;
            }
        }

        return continuation;
    }

    void linux_emulator::setup_hooks()
    {
        // Hook the syscall instruction
        this->emu().hook_instruction(x86_hookable_instructions::syscall,
                                     [this](cpu_interface&, uint64_t) { return this->dispatcher.dispatch(*this); });

        this->emu().hook_interrupt([this](cpu_interface&, const int interrupt) {
            this->notify_interrupt_observers(interrupt);
            if (this->should_stop)
            {
                return;
            }

            if (interrupt == 3)
            {
                const auto trap = resolve_int3_signal_location(this->memory, this->emu().read_instruction_pointer());
                this->emu().reg(x86_register::rip, trap.resume_rip);
                this->signals.deliver_signal(*this, linux_signals::LINUX_SIGTRAP, trap.fault_addr, linux_signals::LINUX_TRAP_BRKPT);
            }
        });

        // Hook memory violations — first give Linux-managed observers a chance
        // to handle the fault, then fall back to SIGSEGV/default termination.
        this->emu().hook_memory_violation([this](cpu_interface&, const uint64_t address, const size_t size,
                                                 const memory_operation operation, const memory_violation_type type) {
            if (!this->memory_violation_observers_.empty())
            {
                return this->notify_memory_violation_observers(address, size, operation, type);
            }

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
            const int si_code =
                (type == memory_violation_type::unmapped) ? linux_signals::LINUX_SEGV_MAPERR : linux_signals::LINUX_SEGV_ACCERR;

            // Try to deliver SIGSEGV to the emulated program's signal handler
            if (this->signals.deliver_signal(*this, linux_signals::LINUX_SIGSEGV, address, si_code))
            {
                // Signal handler was set up — continue execution (it will run the handler)
                return memory_violation_continuation::resume;
            }

            // No handler — default action: terminate
            if (!this->log.is_output_disabled())
            {
                this->log.error("Memory violation at 0x%" PRIx64 ": %s %s (size=%zu, rip=0x%" PRIx64 ")\n", address, type_str, op_str, size,
                                rip);
            }

            this->record_stop(stop_reason::unhandled_memory_violation, format_memory_violation_stop_detail(address, size));
            this->process.exit_status.reset();
            this->stop();
            return memory_violation_continuation::stop;
        });

        // Hook instruction execution (for counting)
        this->emu().hook_memory_execution([this](cpu_interface&, const uint64_t address) { this->on_instruction_execution(address); });
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
        this->last_stop_reason_ = stop_reason::none;
        this->last_stop_detail_.clear();

        const auto use_count = count > 0;
        const auto start_instructions = this->executed_instructions_;
        const auto target_instructions = start_instructions + count;

        std::set<uint64_t> resumed_fetch_faults{};

        while (!this->should_stop)
        {
            try
            {
                if (use_count)
                {
                    const auto current_instructions = this->executed_instructions_;
                    if (current_instructions >= target_instructions)
                    {
                        break;
                    }

                    this->emu().start(static_cast<size_t>(target_instructions - current_instructions));
                }
                else
                {
                    this->emu().start(0);
                }
            }
            catch (const std::exception& e)
            {
                if (this->should_stop || this->last_stop_reason_ != stop_reason::none)
                {
                    break;
                }

                const auto fetch_violation = classify_instruction_fetch_violation(this->memory, this->emu().read_instruction_pointer());
                if (fetch_violation.has_value())
                {
                    const auto address = fetch_violation->address;
                    if (!this->memory_violation_observers_.empty())
                    {
                        const auto continuation =
                            this->notify_memory_violation_observers(address, 1, memory_permission::exec, fetch_violation->type);
                        if (continuation == memory_violation_continuation::restart && !this->should_stop)
                        {
                            continue;
                        }

                        if (continuation == memory_violation_continuation::resume && !this->should_stop)
                        {
                            const auto current_ip = this->emu().read_instruction_pointer();
                            const auto current_violation = classify_instruction_fetch_violation(this->memory, current_ip);
                            if (!current_violation.has_value() && resumed_fetch_faults.insert(current_ip).second)
                            {
                                continue;
                            }
                        }
                    }

                    if (this->last_stop_reason_ == stop_reason::none)
                    {
                        this->record_stop(stop_reason::unhandled_memory_violation, format_memory_violation_stop_detail(address, 1));
                        this->process.exit_status.reset();
                        this->stop();
                    }

                    break;
                }

                this->record_stop(stop_reason::backend_error, e.what());
                throw;
            }
            catch (...)
            {
                this->record_stop(stop_reason::backend_error, "unknown backend error");
                throw;
            }

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

        if (use_count && this->executed_instructions_ >= target_instructions && this->last_stop_reason_ == stop_reason::none)
        {
            this->record_stop(stop_reason::instruction_limit, "count=" + std::to_string(count));
        }
    }

    void linux_emulator::stop()
    {
        if (this->last_stop_reason_ == stop_reason::none)
        {
            this->record_stop(stop_reason::explicit_stop);
        }

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
            {
                this->memory.release_memory(SENTINEL_PAGE, IRELATIVE_PAGE_SIZE);
            }
            if (ok2)
            {
                this->memory.release_memory(RESOLVER_STACK_PAGE, IRELATIVE_PAGE_SIZE);
            }
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
                this->log.warn("IRELATIVE resolver at 0x%" PRIx64 " returned 0 for GOT 0x%" PRIx64 "\n", entry.resolver_addr,
                               entry.got_addr);
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

} // namespace sogen
