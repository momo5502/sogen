#include "std_include.hpp"
#include "windows_emulator.hpp"

#include "cpu_context.hpp"

#include <utils/io.hpp>
#include <utils/finally.hpp>
#include <utils/lazy_object.hpp>

#include "exception_dispatch.hpp"
#include "apiset/apiset.hpp"

#include "network/static_socket_factory.hpp"
#include "memory_permission_ext.hpp"

#include <sstream>

constexpr auto MAX_INSTRUCTIONS_PER_TIME_SLICE = 0x20000;

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
        auto& threads = win_emu.process.threads;
        auto*& active = win_emu.process.active_thread;

        for (auto it = threads.begin(); it != threads.end();)
        {
            if (!it->second.is_terminated() || it->second.ref_count > 0)
            {
                ++it;
                continue;
            }

            if (active == &it->second)
            {
                active = nullptr;
            }

            const auto [new_it, deleted] = threads.erase(it);
            if (!deleted)
            {
                ++it;
            }
            else
            {
                it = new_it;
            }
        }

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

        thread.setup_if_necessary(win_emu.emu(), win_emu.process);

        win_emu.callbacks.on_generic_activity("APC Dispatch");

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
                win_emu.callbacks.on_thread_switch(*active_thread, thread);
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

    std::unique_ptr<utils::clock> get_clock(emulator_interfaces& interfaces, const uint64_t& instructions, const bool use_relative_time)
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

    std::vector<uint8_t> sid_string_to_bytes(const std::string& sid_string)
    {
        if (sid_string.substr(0, 2) != "S-")
        {
            throw std::invalid_argument("invalid SID string");
        }

        std::string token{};
        std::vector<std::string> parts{};
        std::stringstream ss(sid_string.substr(2));

        while (std::getline(ss, token, '-'))
        {
            parts.push_back(token);
        }

        if (parts.size() < 2)
        {
            throw std::invalid_argument("invalid SID string");
        }

        const auto revision = static_cast<uint8_t>(std::stoul(parts[0]));
        const auto authority = std::stoull(parts[1]);
        const auto sub_authority_count = static_cast<uint8_t>(parts.size() - 2);

        std::vector<uint8_t> result;
        result.push_back(revision);
        result.push_back(sub_authority_count);

        for (int i = 5; i >= 0; --i)
        {
            result.push_back((authority >> (i * 8)) & 0xFF);
        }

        for (size_t i = 2; i < parts.size(); ++i)
        {
            uint32_t sub = std::stoul(parts[i]);
            result.push_back((sub >> 0) & 0xFF);
            result.push_back((sub >> 8) & 0xFF);
            result.push_back((sub >> 16) & 0xFF);
            result.push_back((sub >> 24) & 0xFF);
        }

        return result;
    }

    std::string get_user_sid_string(registry_manager& registry)
    {
        constexpr auto profile_list_path = R"(\Registry\Machine\Software\Microsoft\Windows NT\CurrentVersion\ProfileList)";

        const auto profile_list_key = registry.get_key({profile_list_path});
        if (!profile_list_key)
        {
            throw std::runtime_error("Failed to get ProfileList registry key");
        }

        for (size_t i = 0;; ++i)
        {
            const auto value = registry.get_sub_key_name(*profile_list_key, i);
            if (!value.has_value())
            {
                break;
            }

            if (value->starts_with("S-1-5-21-"))
            {
                return std::string(*value);
            }
        }

        return "S-1-5-18";
    }

    std::vector<uint8_t> get_sid(registry_manager& registry)
    {
        const auto sid_string = get_user_sid_string(registry);
        return sid_string_to_bytes(sid_string);
    }
}

windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, application_settings app_settings,
                                   const emulator_settings& settings, emulator_callbacks callbacks, emulator_interfaces interfaces)
    : windows_emulator(std::move(emu), settings, std::move(callbacks), std::move(interfaces))
{
    fixup_application_settings(app_settings);
    this->application_settings_ = std::move(app_settings);
}

windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, const emulator_settings& settings, emulator_callbacks callbacks,
                                   emulator_interfaces interfaces)
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
      use_relative_time_(settings.use_relative_time)
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

    this->setup_hooks();
}

windows_emulator::~windows_emulator() = default;

void windows_emulator::setup_process_if_necessary()
{
    if (!this->application_settings_)
    {
        return;
    }

    auto app_settings = std::move(*this->application_settings_);
    this->application_settings_ = {};

    this->setup_process(app_settings);
}

void windows_emulator::setup_process(const application_settings& app_settings)
{
    const auto& emu = this->emu();
    auto& context = this->process;

    this->sid = get_sid(this->registry);
    this->version.load_from_registry(this->registry, this->log);

    this->mod_manager.map_main_modules(app_settings.application, this->version, context, this->log);

    const auto* executable = this->mod_manager.executable;
    const auto* ntdll = this->mod_manager.ntdll;
    const auto* win32u = this->mod_manager.win32u;

    const auto apiset_data = apiset::obtain(this->emulation_root);

    this->process.setup(this->emu(), this->memory, this->registry, this->file_sys, this->version, app_settings, *executable, *ntdll,
                        apiset_data, this->mod_manager.wow64_modules_.ntdll32);

    const auto ntdll_data = emu.read_memory(ntdll->image_base, static_cast<size_t>(ntdll->size_of_image));
    const auto win32u_data = emu.read_memory(win32u->image_base, static_cast<size_t>(win32u->size_of_image));

    this->dispatcher.setup(ntdll->exports, ntdll_data, win32u->exports, win32u_data);

    const auto main_thread_id = context.create_thread(this->memory, this->mod_manager.executable->entry_point, 0,
                                                      this->mod_manager.executable->size_of_stack_reserve, 0, true);

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

    if (!thread.callback_stack.empty() && address == this->process.zw_callback_return)
    {
        thread.callback_return_rax = this->emu().reg<uint64_t>(x86_register::rax);
    }

    ++this->executed_instructions_;
    const auto thread_insts = ++thread.executed_instructions;
    if (thread_insts % MAX_INSTRUCTIONS_PER_TIME_SLICE == 0)
    {
        this->yield_thread();
    }

    thread.previous_ip = thread.current_ip;
    thread.current_ip = this->emu().read_instruction_pointer();

    this->callbacks.on_instruction(address);
}

void windows_emulator::setup_hooks()
{
    this->emu().hook_instruction(x86_hookable_instructions::syscall, [&] {
        this->dispatcher.dispatch(*this);
        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_instruction(x86_hookable_instructions::rdtscp, [&] {
        this->callbacks.on_rdtscp();

        const auto ticks = this->clock_->timestamp_counter();
        this->emu().reg(x86_register::rax, static_cast<uint32_t>(ticks));
        this->emu().reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

        // Return the IA32_TSC_AUX value in RCX (low 32 bits)
        auto tsc_aux = 0; // Need to replace this with proper CPUID later
        this->emu().reg(x86_register::rcx, tsc_aux);

        return instruction_hook_continuation::skip_instruction;
    });

    this->emu().hook_instruction(x86_hookable_instructions::rdtsc, [&] {
        this->callbacks.on_rdtsc();

        const auto ticks = this->clock_->timestamp_counter();
        this->emu().reg(x86_register::rax, static_cast<uint32_t>(ticks));
        this->emu().reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

        return instruction_hook_continuation::skip_instruction;
    });

    // TODO: Unicorn needs this - This should be handled in the backend
    this->emu().hook_instruction(x86_hookable_instructions::invalid, [&] {
        // TODO: Unify icicle & unicorn handling
        dispatch_illegal_instruction_violation(*this);
        return instruction_hook_continuation::skip_instruction; //
    });

    this->emu().hook_interrupt([&](const int interrupt) {
        this->callbacks.on_exception();
        const auto eflags = this->emu().reg<uint32_t>(x86_register::eflags);

        switch (interrupt)
        {
        case 0:
            dispatch_integer_division_by_zero(*this);
            return;
        case 1:
            if ((eflags & 0x100) != 0)
            {
                this->emu().reg(x86_register::eflags, eflags & ~0x100);
            }

            this->callbacks.on_suspicious_activity("Singlestep");
            dispatch_single_step(*this);
            return;
        case 3:
            this->callbacks.on_suspicious_activity("Breakpoint");
            dispatch_breakpoint(*this);
            return;
        case 6:
            this->callbacks.on_suspicious_activity("Illegal instruction");
            dispatch_illegal_instruction_violation(*this);
            return;
        case 45:
            this->callbacks.on_suspicious_activity("DbgPrint");
            dispatch_breakpoint(*this);
            return;
        default:
            if (this->callbacks.on_generic_activity)
            {
                this->callbacks.on_generic_activity("Interrupt " + std::to_string(interrupt));
            }

            break;
        }
    });

    this->emu().hook_memory_violation(
        [&](const uint64_t address, const size_t size, const memory_operation operation, const memory_violation_type type) {
            if (this->emu().reg<uint16_t>(x86_register::cs) == 0x33)
            {
                // loading gs selector only works in 64-bit mode
                const auto required_gs_base = this->current_thread().gs_segment->get_base();
                const auto actual_gs_base = this->emu().get_segment_base(x86_register::gs);
                if (actual_gs_base != required_gs_base)
                {
                    this->emu().set_segment_base(x86_register::gs, required_gs_base);
                    return memory_violation_continuation::restart;
                }
            }

            auto region = this->memory.get_region_info(address);
            if (region.permissions.is_guarded())
            {
                // Unset the GUARD_PAGE flag and dispatch a STATUS_GUARD_PAGE_VIOLATION
                this->memory.protect_memory(region.allocation_base, region.length, region.permissions & ~memory_permission_ext::guard);
                dispatch_guard_page_violation(*this, address, operation);
            }
            else
            {
                this->callbacks.on_memory_violate(address, size, operation, type);
                dispatch_access_violation(*this, address, operation);
            }

            return memory_violation_continuation::resume;
        });

    this->emu().hook_memory_execution([&](const uint64_t address) {
        this->on_instruction_execution(address); //
    });
}

void windows_emulator::start(size_t count)
{
    this->should_stop = false;
    this->setup_process_if_necessary();

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

    buffer.register_factory<window>([this] {
        return window{this->emu()}; //
    });
}

void windows_emulator::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write_vector(this->sid);
    buffer.write_optional(this->application_settings_);
    buffer.write(this->executed_instructions_);
    buffer.write(this->switch_thread_);
    buffer.write(this->use_relative_time_);

    this->version.serialize(buffer);
    this->registry.serialize_runtime_state(buffer);

    this->emu().serialize_state(buffer, false);
    this->memory.serialize_memory_state(buffer, false);
    this->mod_manager.serialize(buffer);
    this->dispatcher.serialize(buffer);
    this->process.serialize(buffer);
}

void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
{
    this->register_factories(buffer);

    buffer.read_vector(this->sid);
    buffer.read_optional(this->application_settings_);
    buffer.read(this->executed_instructions_);
    buffer.read(this->switch_thread_);

    const auto old_relative_time = this->use_relative_time_;
    buffer.read(this->use_relative_time_);

    if (old_relative_time != this->use_relative_time_)
    {
        throw std::runtime_error("Can not deserialize emulator with different time dimensions");
    }

    this->version.deserialize(buffer);
    this->registry.deserialize_runtime_state(buffer);

    this->memory.unmap_all_memory();

    this->emu().deserialize_state(buffer, false);
    this->memory.deserialize_memory_state(buffer, false);
    this->mod_manager.deserialize(buffer);
    this->dispatcher.deserialize(buffer);
    this->process.deserialize(buffer);
}

void windows_emulator::save_snapshot()
{
    utils::buffer_serializer buffer{};

    buffer.write_optional(this->application_settings_);
    buffer.write(this->executed_instructions_);
    buffer.write(this->switch_thread_);

    this->version.serialize(buffer);
    this->registry.serialize_runtime_state(buffer);

    this->emu().serialize_state(buffer, true);
    this->memory.serialize_memory_state(buffer, true);
    this->mod_manager.serialize(buffer);
    this->process.serialize(buffer);

    this->process_snapshot_ = buffer.move_buffer();

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

    utils::buffer_deserializer buffer{this->process_snapshot_};

    this->register_factories(buffer);

    buffer.read_optional(this->application_settings_);
    buffer.read(this->executed_instructions_);
    buffer.read(this->switch_thread_);

    this->version.deserialize(buffer);
    this->registry.deserialize_runtime_state(buffer);

    this->emu().deserialize_state(buffer, true);
    this->memory.deserialize_memory_state(buffer, true);
    this->mod_manager.deserialize(buffer);
    this->process.deserialize(buffer);
    // this->process = *this->process_snapshot_;
}
