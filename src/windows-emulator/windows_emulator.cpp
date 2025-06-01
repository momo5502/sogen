#include "std_include.hpp"
#include "windows_emulator.hpp"

#include "cpu_context.hpp"

#include <utils/io.hpp>
#include <utils/finally.hpp>
#include <utils/lazy_object.hpp>

#include "exception_dispatch.hpp"
#include "apiset/apiset.hpp"

#include "network/static_socket_factory.hpp"

constexpr auto MAX_INSTRUCTIONS_PER_TIME_SLICE = 100000;

namespace
{
    void adjust_working_directory(application_settings& app_settings)
    {
        if (!app_settings.working_directory.empty())
        {
            // Do nothing
        }
#ifdef OS_WINDOWS
        else if (app_settings.application.is_relative())
        {
            app_settings.working_directory = std::filesystem::current_path();
        }
#endif
        else
        {
            app_settings.working_directory = app_settings.application.parent();
        }
    }

    void adjust_application(application_settings& app_settings)
    {
        if (app_settings.application.is_relative())
        {
            app_settings.application = app_settings.working_directory / app_settings.application;
        }
    }

    void fixup_application_settings(application_settings& app_settings)
    {
        adjust_working_directory(app_settings);
        adjust_application(app_settings);
    }

    void perform_context_switch_work(windows_emulator& win_emu)
    {
        auto& devices = win_emu.process.devices;

        // Crappy mechanism to prevent mutation while iterating.
        const auto was_blocked = devices.block_mutation(true);
        const auto _ = utils::finally([&] { devices.block_mutation(was_blocked); });

        for (auto& dev : devices | std::views::values)
        {
            dev.work(win_emu);
        }
    }

    emulator_thread* get_thread_by_id(process_context& process, const uint32_t id)
    {
        for (auto& t : process.threads | std::views::values)
        {
            if (t.id == id)
            {
                return &t;
            }
        }

        return nullptr;
    }

    void dispatch_next_apc(windows_emulator& win_emu, emulator_thread& thread)
    {
        assert(&win_emu.current_thread() == &thread);

        auto& emu = win_emu.emu();
        auto& apcs = thread.pending_apcs;
        if (apcs.empty())
        {
            return;
        }

        win_emu.log.print(color::dark_gray, "Dispatching APC...\n");

        const auto next_apx = apcs.front();
        apcs.erase(apcs.begin());

        struct
        {
            CONTEXT64 context{};
            CONTEXT_EX context_ex{};
            KCONTINUE_ARGUMENT continue_argument{};
        } stack_layout;

        static_assert(offsetof(decltype(stack_layout), continue_argument) == 0x4F0);

        stack_layout.context.P1Home = next_apx.apc_argument1;
        stack_layout.context.P2Home = next_apx.apc_argument2;
        stack_layout.context.P3Home = next_apx.apc_argument3;
        stack_layout.context.P4Home = next_apx.apc_routine;

        stack_layout.continue_argument.ContinueFlags |= KCONTINUE_FLAG_TEST_ALERT;

        auto& ctx = stack_layout.context;
        ctx.ContextFlags = CONTEXT64_ALL;
        cpu_context::save(emu, ctx);

        const auto initial_sp = emu.reg(x86_register::rsp);
        const auto new_sp = align_down(initial_sp - sizeof(stack_layout), 0x100);

        emu.write_memory(new_sp, stack_layout);

        emu.reg(x86_register::rsp, new_sp);
        emu.reg(x86_register::rip, win_emu.process.ki_user_apc_dispatcher);
    }

    bool switch_to_thread(windows_emulator& win_emu, emulator_thread& thread, const bool force = false)
    {
        if (thread.is_terminated())
        {
            return false;
        }

        auto& emu = win_emu.emu();
        auto& context = win_emu.process;

        const auto is_ready = thread.is_thread_ready(context, win_emu.clock());
        const auto can_dispatch_apcs = thread.apc_alertable && !thread.pending_apcs.empty();

        if (!is_ready && !force && !can_dispatch_apcs)
        {
            return false;
        }

        auto* active_thread = context.active_thread;

        if (active_thread != &thread)
        {
            if (active_thread)
            {
                win_emu.log.print(color::dark_gray, "Performing thread switch: %X -> %X\n", active_thread->id,
                                  thread.id);
                active_thread->save(emu);
            }

            context.active_thread = &thread;

            thread.restore(emu);
        }

        thread.setup_if_necessary(emu, context);

        if (can_dispatch_apcs)
        {
            thread.mark_as_ready(STATUS_USER_APC);
            dispatch_next_apc(win_emu, thread);
        }

        thread.apc_alertable = false;
        win_emu.callbacks.on_thread_switch();
        return true;
    }

    bool switch_to_thread(windows_emulator& win_emu, const handle thread_handle)
    {
        auto* thread = win_emu.process.threads.get(thread_handle);
        if (!thread)
        {
            throw std::runtime_error("Bad thread handle");
        }

        return switch_to_thread(win_emu, *thread);
    }

    bool switch_to_next_thread(windows_emulator& win_emu)
    {
        perform_context_switch_work(win_emu);

        auto& context = win_emu.process;

        bool next_thread = false;

        for (auto& t : context.threads | std::views::values)
        {
            if (next_thread)
            {
                if (switch_to_thread(win_emu, t))
                {
                    return true;
                }

                continue;
            }

            if (&t == context.active_thread)
            {
                next_thread = true;
            }
        }

        for (auto& t : context.threads | std::views::values)
        {
            if (switch_to_thread(win_emu, t))
            {
                return true;
            }
        }

        return false;
    }

    struct instruction_tick_clock : utils::tick_clock
    {
        const uint64_t* instructions_{};

        instruction_tick_clock(const uint64_t& instructions, const system_time_point system_start = {},
                               const steady_time_point steady_start = {})
            : tick_clock(1000, system_start, steady_start),
              instructions_(&instructions)
        {
        }

        uint64_t ticks() override
        {
            return *this->instructions_;
        }
    };

    std::unique_ptr<utils::clock> get_clock(emulator_interfaces& interfaces, const uint64_t& instructions,
                                            const bool use_relative_time)
    {
        if (interfaces.clock)
        {
            return std::move(interfaces.clock);
        }

        if (use_relative_time)
        {
            return std::make_unique<instruction_tick_clock>(instructions);
        }

        return std::make_unique<utils::clock>();
    }
    std::unique_ptr<network::socket_factory> get_socket_factory(emulator_interfaces& interfaces)
    {
        if (interfaces.socket_factory)
        {
            return std::move(interfaces.socket_factory);
        }

#ifdef OS_EMSCRIPTEN
        return network::create_static_socket_factory();
#else
        return std::make_unique<network::socket_factory>();
#endif
    }
}

windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, application_settings app_settings,
                                   const emulator_settings& settings, emulator_callbacks callbacks,
                                   emulator_interfaces interfaces)
    : windows_emulator(std::move(emu), settings, std::move(callbacks), std::move(interfaces))
{
    fixup_application_settings(app_settings);
    this->setup_process(app_settings);
}

windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, const emulator_settings& settings,
                                   emulator_callbacks callbacks, emulator_interfaces interfaces)
    : emu_(std::move(emu)),
      clock_(get_clock(interfaces, this->executed_instructions_, settings.use_relative_time)),
      socket_factory_(get_socket_factory(interfaces)),
      emulation_root{settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root)},
      callbacks(std::move(callbacks)),
      file_sys(emulation_root.empty() ? emulation_root : emulation_root / "filesys"),
      memory(*this->emu_),
      registry(emulation_root.empty() ? settings.registry_directory : emulation_root / "registry"),
      mod_manager(memory, file_sys, this->callbacks),
      process(*this->emu_, memory, *this->clock_, this->callbacks),
      modules_(settings.modules),
      ignored_functions_(settings.ignored_functions)
{
#ifndef OS_WINDOWS
    if (this->emulation_root.empty())
    {
        throw std::runtime_error("Emulation root directory can not be empty!");
    }
#endif

    for (const auto& mapping : settings.path_mappings)
    {
        this->file_sys.map(mapping.first, mapping.second);
    }

    for (const auto& mapping : settings.port_mappings)
    {
        this->map_port(mapping.first, mapping.second);
    }

    this->verbose_calls = settings.verbose_calls;
    this->silent_until_main_ = settings.silent_until_main && !settings.disable_logging;
    this->use_relative_time_ = settings.use_relative_time;
    this->log.disable_output(settings.disable_logging || this->silent_until_main_);

    this->setup_hooks();
}

windows_emulator::~windows_emulator() = default;

void windows_emulator::setup_process(const application_settings& app_settings)
{
    const auto& emu = this->emu();
    auto& context = this->process;

    this->mod_manager.map_main_modules(app_settings.application, R"(C:\Windows\System32\ntdll.dll)",
                                       R"(C:\Windows\System32\win32u.dll)", this->log);

    const auto* executable = this->mod_manager.executable;
    const auto* ntdll = this->mod_manager.ntdll;
    const auto* win32u = this->mod_manager.win32u;

    const auto apiset_data = apiset::obtain(this->emulation_root);

    this->process.setup(this->emu(), this->memory, this->registry, app_settings, *executable, *ntdll, apiset_data);

    const auto ntdll_data = emu.read_memory(ntdll->image_base, static_cast<size_t>(ntdll->size_of_image));
    const auto win32u_data = emu.read_memory(win32u->image_base, static_cast<size_t>(win32u->size_of_image));

    this->dispatcher.setup(ntdll->exports, ntdll_data, win32u->exports, win32u_data);

    const auto main_thread_id =
        context.create_thread(this->memory, this->mod_manager.executable->entry_point, 0, 0, false);
    switch_to_thread(*this, main_thread_id);
}

void windows_emulator::yield_thread(const bool alertable)
{
    this->switch_thread_ = true;
    this->current_thread().apc_alertable = alertable;
    this->emu().stop();
}

bool windows_emulator::perform_thread_switch()
{
    const auto needed_switch = std::exchange(this->switch_thread_, false);

    this->switch_thread_ = false;
    while (!switch_to_next_thread(*this))
    {
        if (this->use_relative_time_)
        {
            this->executed_instructions_ += MAX_INSTRUCTIONS_PER_TIME_SLICE;
        }
        else
        {
            std::this_thread::sleep_for(1ms);
        }

        if (this->should_stop)
        {
            this->switch_thread_ = needed_switch;
            return false;
        }
    }

    return true;
}

bool windows_emulator::activate_thread(const uint32_t id)
{
    auto* thread = get_thread_by_id(this->process, id);
    if (!thread)
    {
        return false;
    }

    return switch_to_thread(*this, *thread, true);
}

void windows_emulator::on_instruction_execution(const uint64_t address)
{
    auto& thread = this->current_thread();

    ++this->executed_instructions_;
    const auto thread_insts = ++thread.executed_instructions;
    if (thread_insts % MAX_INSTRUCTIONS_PER_TIME_SLICE == 0)
    {
        this->yield_thread();
    }

    this->process.previous_ip = this->process.current_ip;
    this->process.current_ip = this->emu().read_instruction_pointer();

    const auto is_main_exe = this->mod_manager.executable->is_within(address);
    const auto is_previous_main_exe = this->mod_manager.executable->is_within(this->process.previous_ip);

    const auto binary = utils::make_lazy([&] {
        if (is_main_exe)
        {
            return this->mod_manager.executable;
        }

        return this->mod_manager.find_by_address(address); //
    });

    const auto previous_binary = utils::make_lazy([&] {
        if (is_previous_main_exe)
        {
            return this->mod_manager.executable;
        }

        return this->mod_manager.find_by_address(this->process.previous_ip); //
    });

    const auto is_in_interesting_module = [&] {
        if (this->modules_.empty())
        {
            return false;
        }

        return (binary && this->modules_.contains(binary->name)) ||
               (previous_binary && this->modules_.contains(previous_binary->name));
    };

    const auto is_interesting_call = is_previous_main_exe //
                                     || is_main_exe       //
                                     || is_in_interesting_module();

    if (this->silent_until_main_ && is_main_exe)
    {
        this->silent_until_main_ = false;
        this->log.disable_output(false);
    }

    if (!this->verbose && !this->verbose_calls && !is_interesting_call)
    {
        return;
    }

    if (binary)
    {
        const auto export_entry = binary->address_names.find(address);
        if (export_entry != binary->address_names.end() && !this->ignored_functions_.contains(export_entry->second))
        {
            const auto rsp = this->emu().read_stack_pointer();

            uint64_t return_address{};
            this->emu().try_read_memory(rsp, &return_address, sizeof(return_address));

            const auto* mod_name = this->mod_manager.find_name(return_address);

            log.print(is_interesting_call ? color::yellow : color::dark_gray,
                      "Executing function: %s - %s (0x%" PRIx64 ") via (0x%" PRIx64 ") %s\n", binary->name.c_str(),
                      export_entry->second.c_str(), address, return_address, mod_name);
        }
        else if (address == binary->entry_point)
        {
            log.print(is_interesting_call ? color::yellow : color::gray, "Executing entry point: %s (0x%" PRIx64 ")\n",
                      binary->name.c_str(), address);
        }
    }

    if (!this->verbose)
    {
        return;
    }

    auto& emu = this->emu();

    // TODO: Remove or cleanup
    log.print(color::gray,
              "Inst: %16" PRIx64 " - RAX: %16" PRIx64 " - RBX: %16" PRIx64 " - RCX: %16" PRIx64 " - RDX: %16" PRIx64
              " - R8: %16" PRIx64 " - R9: %16" PRIx64 " - RDI: %16" PRIx64 " - RSI: %16" PRIx64 " - %s\n",
              address, emu.reg(x86_register::rax), emu.reg(x86_register::rbx), emu.reg(x86_register::rcx),
              emu.reg(x86_register::rdx), emu.reg(x86_register::r8), emu.reg(x86_register::r9),
              emu.reg(x86_register::rdi), emu.reg(x86_register::rsi), binary ? binary->name.c_str() : "<N/A>");
}

void windows_emulator::setup_hooks()
{
    this->emu().hook_instruction(x86_hookable_instructions::syscall, [&] {
        this->dispatcher.dispatch(*this);
        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_instruction(x86_hookable_instructions::rdtscp, [&] {
        const auto ticks = this->clock_->timestamp_counter();
        this->emu().reg(x86_register::rax, ticks & 0xFFFFFFFF);
        this->emu().reg(x86_register::rdx, (ticks >> 32) & 0xFFFFFFFF);

        // Return the IA32_TSC_AUX value in RCX (low 32 bits)
        auto tsc_aux = 0; // Need to replace this with proper CPUID later
        this->emu().reg(x86_register::rcx, tsc_aux);

        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_instruction(x86_hookable_instructions::rdtsc, [&] {
        const auto ticks = this->clock_->timestamp_counter();
        this->emu().reg(x86_register::rax, ticks & 0xFFFFFFFF);
        this->emu().reg(x86_register::rdx, (ticks >> 32) & 0xFFFFFFFF);
        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_instruction(x86_hookable_instructions::invalid, [&] {
        const auto ip = this->emu().read_instruction_pointer();

        this->log.print(color::gray, "Invalid instruction at: 0x%" PRIx64 " (via 0x%" PRIx64 ")\n", ip,
                        this->process.previous_ip);

        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_interrupt([&](const int interrupt) {
        const auto rip = this->emu().read_instruction_pointer();
        const auto eflags = this->emu().reg<uint32_t>(x86_register::eflags);

        switch (interrupt)
        {
        case 0:
            dispatch_integer_division_by_zero(this->emu(), this->process);
            return;
        case 1:
            if ((eflags & 0x100) != 0)
            {
                this->log.print(color::pink, "Singlestep (Trap Flag): 0x%" PRIx64 "\n", rip);
                this->emu().reg(x86_register::eflags, eflags & ~0x100);
            }
            else
            {
                this->log.print(color::pink, "Singlestep: 0x%" PRIx64 "\n", rip);
            }
            dispatch_single_step(this->emu(), this->process);
            return;
        case 3:
            this->log.print(color::pink, "Breakpoint: 0x%" PRIx64 "\n", rip);
            dispatch_breakpoint(this->emu(), this->process);
            return;
        case 6:
            dispatch_illegal_instruction_violation(this->emu(), this->process);
            return;
        case 45:
            this->log.print(color::pink, "DbgPrint: 0x%" PRIx64 "\n", rip);
            dispatch_breakpoint(this->emu(), this->process);
            return;
        default:
            break;
        }

        this->log.print(color::gray, "Interrupt: %i 0x%" PRIx64 "\n", interrupt, rip);

        if (this->fuzzing || true) // TODO: Fix
        {
            this->process.exception_rip = rip;
            this->emu().stop();
        }
    });

    this->emu().hook_memory_violation([&](const uint64_t address, const size_t size, const memory_operation operation,
                                          const memory_violation_type type) {
        const auto permission = get_permission_string(operation);
        const auto ip = this->emu().read_instruction_pointer();
        const char* name = this->mod_manager.find_name(ip);

        if (type == memory_violation_type::protection)
        {
            this->log.print(color::gray, "Protection violation: 0x%" PRIx64 " (%zX) - %s at 0x%" PRIx64 " (%s)\n",
                            address, size, permission.c_str(), ip, name);
        }
        else if (type == memory_violation_type::unmapped)
        {
            this->log.print(color::gray, "Mapping violation: 0x%" PRIx64 " (%zX) - %s at 0x%" PRIx64 " (%s)\n", address,
                            size, permission.c_str(), ip, name);
        }

        if (this->fuzzing)
        {
            this->process.exception_rip = ip;
            this->emu().stop();
            return memory_violation_continuation::stop;
        }

        dispatch_access_violation(this->emu(), this->process, address, operation);
        return memory_violation_continuation::resume;
    });

    this->emu().hook_memory_execution([&](const uint64_t address) {
        this->on_instruction_execution(address); //
    });
}

void windows_emulator::start(size_t count)
{
    this->should_stop = false;

    const auto use_count = count > 0;
    const auto start_instructions = this->executed_instructions_;
    const auto target_instructions = start_instructions + count;

    while (!this->should_stop)
    {
        if (this->switch_thread_ || !this->current_thread().is_thread_ready(this->process, this->clock()))
        {
            if (!this->perform_thread_switch())
            {
                break;
            }
        }

        this->emu().start(count);

        if (!this->switch_thread_ && !this->emu().has_violation())
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

            count = static_cast<size_t>(target_instructions - current_instructions);
        }
    }
}

void windows_emulator::stop()
{
    this->should_stop = true;
    this->emu().stop();
}

void windows_emulator::register_factories(utils::buffer_deserializer& buffer)
{
    buffer.register_factory<memory_manager_wrapper>([this] {
        return memory_manager_wrapper{this->memory}; //
    });

    buffer.register_factory<module_manager_wrapper>([this] {
        return module_manager_wrapper{this->mod_manager}; //
    });

    buffer.register_factory<x64_emulator_wrapper>([this] {
        return x64_emulator_wrapper{this->emu()}; //
    });

    buffer.register_factory<windows_emulator_wrapper>([this] {
        return windows_emulator_wrapper{*this}; //
    });

    buffer.register_factory<clock_wrapper>([this] {
        return clock_wrapper{this->clock()}; //
    });

    buffer.register_factory<socket_factory_wrapper>([this] {
        return socket_factory_wrapper{this->socket_factory()}; //
    });
}

void windows_emulator::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->executed_instructions_);
    buffer.write(this->switch_thread_);
    buffer.write(this->use_relative_time_);
    this->emu().serialize_state(buffer, false);
    this->memory.serialize_memory_state(buffer, false);
    this->mod_manager.serialize(buffer);
    this->process.serialize(buffer);
    this->dispatcher.serialize(buffer);
}

void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
{
    this->register_factories(buffer);

    buffer.read(this->executed_instructions_);
    buffer.read(this->switch_thread_);

    const auto old_relative_time = this->use_relative_time_;
    buffer.read(this->use_relative_time_);

    if (old_relative_time != this->use_relative_time_)
    {
        throw std::runtime_error("Can not deserialize emulator with different time dimensions");
    }

    this->memory.unmap_all_memory();

    this->emu().deserialize_state(buffer, false);
    this->memory.deserialize_memory_state(buffer, false);
    this->mod_manager.deserialize(buffer);
    this->process.deserialize(buffer);
    this->dispatcher.deserialize(buffer);
}

void windows_emulator::save_snapshot()
{
    utils::buffer_serializer serializer{};
    this->emu().serialize_state(serializer, true);
    this->memory.serialize_memory_state(serializer, true);
    this->mod_manager.serialize(serializer);
    this->process.serialize(serializer);

    this->process_snapshot_ = serializer.move_buffer();

    // TODO: Make process copyable
    // this->process_snapshot_ = this->process;
}

void windows_emulator::restore_snapshot()
{
    if (this->process_snapshot_.empty())
    {
        assert(false);
        return;
    }

    utils::buffer_deserializer deserializer{this->process_snapshot_};

    this->register_factories(deserializer);

    this->emu().deserialize_state(deserializer, true);
    this->memory.deserialize_memory_state(deserializer, true);
    this->mod_manager.deserialize(deserializer);
    this->process.deserialize(deserializer);
    // this->process = *this->process_snapshot_;
}
