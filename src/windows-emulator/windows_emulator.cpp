#include "std_include.hpp"
#include "windows_emulator.hpp"

#include "cpu_context.hpp"

#include <utils/io.hpp>
#include <utils/timer.hpp>
#include <utils/finally.hpp>
#include <utils/lazy_object.hpp>

#include "exception_dispatch.hpp"
#include "apiset/apiset.hpp"
#include "syscall_dispatcher.hpp"

#include "network/static_socket_factory.hpp"
#include "memory_permission_ext.hpp"

namespace sogen
{
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

        int16_t point_x(const uint64_t lparam)
        {
            return static_cast<int16_t>(lparam & 0xFFFF);
        }

        int16_t point_y(const uint64_t lparam)
        {
            return static_cast<int16_t>((lparam >> 16) & 0xFFFF);
        }

        struct child_hit_test_result
        {
            const window* win{};
            int x{};
            int y{};
        };

        std::optional<child_hit_test_result> find_child_window_at(const process_context& process, const hwnd parent, const int x,
                                                                  const int y)
        {
            std::optional<child_hit_test_result> result{};
            for (const auto& [index, child] : process.windows)
            {
                (void)index;
                if (child.parent_handle != parent || (child.style & WS_VISIBLE) == 0 || (child.style & WS_DISABLED) != 0)
                {
                    continue;
                }

                if (x >= child.x && x < child.x + child.width && y >= child.y && y < child.y + child.height)
                {
                    const auto child_x = x - child.x;
                    const auto child_y = y - child.y;
                    if (auto descendant = find_child_window_at(process, child.handle, child_x, child_y))
                    {
                        result = descendant;
                    }
                    else
                    {
                        result = child_hit_test_result{.win = &child, .x = child_x, .y = child_y};
                    }
                }
            }

            return result;
        }

        std::optional<POINT> get_window_origin_relative_to_ancestor(const process_context& process, const hwnd window, const hwnd ancestor)
        {
            POINT origin{};
            auto current_handle = window;
            while (current_handle != 0 && current_handle != ancestor)
            {
                const auto* current = process.windows.get(current_handle);
                if (!current)
                {
                    return std::nullopt;
                }

                origin.x += current->x;
                origin.y += current->y;
                current_handle = current->parent_handle;
            }

            if (current_handle != ancestor)
            {
                return std::nullopt;
            }

            return origin;
        }

        bool is_pointer_message(const uint32_t message)
        {
            // All mouse messages go through capture/child hit-testing: while a window holds the mouse
            // capture every mouse message must reach it (so a pressed button still completes its click),
            // and otherwise each is delivered to the child under the cursor (hover, right-click, etc.).
            return message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_RBUTTONDOWN ||
                   message == WM_RBUTTONUP;
        }

        uint64_t pack_point(const int x, const int y)
        {
            return static_cast<uint16_t>(x) | (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16);
        }

        struct pointer_target
        {
            hwnd window{};
            int x{};
            int y{};
        };

        // Single authority for routing a top-level-local pointer event to its destination window.
        // Backends only forward (top-level window, top-level-local x/y); capture and child hit-testing
        // are decided here, never in the host backends.
        pointer_target route_pointer(process_context& process, const hwnd top_level, const int x, const int y)
        {
            if (process.mouse_capture_window != 0)
            {
                if (const auto* captured = process.windows.get(process.mouse_capture_window);
                    captured && (captured->style & WS_VISIBLE) != 0)
                {
                    // Capture sends every pointer event to the capturing window, even one reported for
                    // another top-level. Translate via screen coordinates (origin relative to the root)
                    // so it works across top-levels; for a child of top_level this is the same offset.
                    const auto captured_origin = get_window_origin_relative_to_ancestor(process, captured->handle, 0);
                    const auto top_level_origin = get_window_origin_relative_to_ancestor(process, top_level, 0);
                    if (captured_origin && top_level_origin)
                    {
                        const auto screen_x = top_level_origin->x + x;
                        const auto screen_y = top_level_origin->y + y;
                        return {.window = captured->handle, .x = screen_x - captured_origin->x, .y = screen_y - captured_origin->y};
                    }
                }
                else
                {
                    process.mouse_capture_window = 0;
                }

                return {.window = top_level, .x = x, .y = y};
            }

            // Otherwise deliver to the deepest visible/enabled child under the cursor.
            if (const auto child = find_child_window_at(process, top_level, x, y))
            {
                return {.window = child->win->handle, .x = child->x, .y = child->y};
            }

            return {.window = top_level, .x = x, .y = y};
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

            if (next_apx.restamp_io_status_block && next_apx.apc_argument2)
            {
                // Stamp the WoW64 32-bit I/O status block (Status @0, Information @4) here, immediately
                // before the completion routine runs. This mirrors the real kernel, which writes the
                // IO_STATUS_BLOCK as it dispatches the completion APC rather than when the I/O is first
                // queued -- so any value the guest left in that buffer while the async request was pending
                // (e.g. reusing it for an intervening synchronous call) is correctly superseded.
                const auto status32 = static_cast<uint32_t>(next_apx.io_status);
                emu.write_memory(next_apx.apc_argument2, &status32, sizeof(status32));
                emu.write_memory(next_apx.apc_argument2 + sizeof(status32), &next_apx.io_information, sizeof(next_apx.io_information));
            }

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
            const auto has_pending_status = thread.pending_status.has_value();
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

            if (can_dispatch_apcs && !has_pending_status)
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

        // Diagnostic (EMULATOR_LOG_STACKS): for every live WoW64 thread, scan its committed 32-bit stack
        // (bounds from the TEB, so no exact ESP needed) and print the values that are genuine return
        // addresses (resolve to a loaded module and are preceded by a CALL). The real call chain stands out
        // among the stale entries. A warmup skips early init so the timing-sensitive display setup isn't
        // perturbed.
        void log_thread_stacks(windows_emulator& win_emu)
        {
            static const bool enabled = std::getenv("EMULATOR_LOG_STACKS") != nullptr;
            if (!enabled)
            {
                return;
            }
            static const auto begin = std::chrono::steady_clock::now();
            static std::chrono::steady_clock::time_point last{};
            const auto now = std::chrono::steady_clock::now();
            if (now - begin < std::chrono::seconds(60) || now - last < std::chrono::seconds(3))
            {
                return;
            }
            last = now;

            auto& emu = win_emu.emu();
            auto& mods = win_emu.mod_manager;
            const auto is_return = [&](const uint32_t value) -> bool {
                uint8_t b[8] = {};
                if (!emu.try_read_memory(static_cast<uint64_t>(value) - 7, b, sizeof(b)))
                {
                    return false;
                }
                if (b[2] == 0xE8) // E8 rel32 (5 bytes), opcode at value-5
                {
                    return true;
                }
                for (const int len : {2, 3, 4, 6, 7}) // FF /2 (call r/m), opcode at value-len
                {
                    const int idx = 7 - len;
                    if (b[idx] == 0xFF && ((b[idx + 1] >> 3) & 7) == 2)
                    {
                        return true;
                    }
                }
                return false;
            };

            win_emu.log.print(color::cyan, "=== thread stacks ===\n");
            for (auto& t : win_emu.process.threads | std::views::values)
            {
                if (t.is_terminated() || !t.teb32)
                {
                    continue;
                }
                const auto tib = t.teb32->read().NtTib;
                const uint64_t stack_base = tib.StackBase;
                const uint64_t stack_limit = tib.StackLimit;
                if (stack_base <= stack_limit || stack_base - stack_limit > 0x400000)
                {
                    continue;
                }
                // Scan only the live region above the thread's 32-bit ESP; everything below it is popped
                // frames whose stale return addresses would otherwise pollute the chain. wow64cpu saves the
                // 32-bit context (incl. ESP) into WOW64_CPURESERVED on each transition, so for a thread
                // parked in a syscall this is the live stack pointer at the wait.
                uint64_t scan_floor = stack_limit;
                if (t.wow64_cpu_reserved.has_value())
                {
                    const uint64_t sp = t.wow64_cpu_reserved->read().Context.Esp;
                    if (sp > stack_limit && sp < stack_base)
                    {
                        scan_floor = sp;
                    }
                }
                win_emu.log.print(color::cyan, "-- tid %u '%s' stack 0x%llx-0x%llx sp=0x%llx --\n", t.id, u16_to_u8(t.name).c_str(),
                                  static_cast<unsigned long long>(stack_limit), static_cast<unsigned long long>(stack_base),
                                  static_cast<unsigned long long>(scan_floor));
                for (uint64_t addr = stack_base - 4; addr >= scan_floor; addr -= 4)
                {
                    uint32_t value = 0;
                    if (!emu.try_read_memory(addr, &value, sizeof(value)))
                    {
                        continue;
                    }
                    const auto* mod = mods.find_by_address(value);
                    if (!mod || !is_return(value))
                    {
                        continue;
                    }
                    win_emu.log.print(color::cyan, "   0x%llx: 0x%x (%s+0x%llx)\n", static_cast<unsigned long long>(addr), value,
                                      mod->name.c_str(), static_cast<unsigned long long>(static_cast<uint64_t>(value) - mod->image_base));
                }
            }
        }

        const char* handle_type_name(const handle h)
        {
            switch (static_cast<handle_types::type>(h.value.type))
            {
            case handle_types::event:
                return "event";
            case handle_types::semaphore:
                return "semaphore";
            case handle_types::mutant:
                return "mutant";
            case handle_types::timer:
                return "timer";
            case handle_types::thread:
                return "thread";
            case handle_types::process:
                return "process";
            case handle_types::file:
                return "file";
            case handle_types::io_completion:
                return "iocp";
            default:
                return "object";
            }
        }

        // Diagnostic (EMULATOR_LOG_WAITS): for every live thread, print what it is currently blocked on
        // (object wait, alert wait, sleep, message wait, I/O completion, suspended) or that it is runnable,
        // plus its instruction pointer resolved to a module. Reveals which thread owns a busy-loop and what
        // the rest are parked on when the guest appears stuck.
        void log_thread_waits(windows_emulator& win_emu)
        {
            static const bool enabled = std::getenv("EMULATOR_LOG_WAITS") != nullptr;
            if (!enabled)
            {
                return;
            }
            static std::chrono::steady_clock::time_point last{};
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(2))
            {
                return;
            }
            last = now;

            auto& mods = win_emu.mod_manager;
            win_emu.log.print(color::yellow, "=== thread waits ===\n");
            for (auto& t : win_emu.process.threads | std::views::values)
            {
                if (t.is_terminated())
                {
                    continue;
                }

                std::string state;
                if (t.suspended)
                {
                    state = "SUSPENDED";
                }
                else if (!t.await_objects.empty())
                {
                    constexpr auto infinite = std::chrono::steady_clock::time_point::min();
                    const bool timed = t.await_time.has_value() && t.await_time.value() != infinite;
                    state = t.await_any ? "WAIT-ANY[" : "WAIT-ALL[";
                    for (size_t i = 0; i < t.await_objects.size(); ++i)
                    {
                        if (i != 0)
                        {
                            state += ',';
                        }
                        state += handle_type_name(t.await_objects[i]);
                        state += ':';
                        state += utils::string::to_hex_number(t.await_objects[i].value.id);
                    }
                    state += timed ? "]+timeout" : "]";
                }
                else if (t.waiting_for_alert)
                {
                    state = t.alerted ? "ALERTED" : "ALERTWAIT";
                }
                else if (t.await_msg.has_value())
                {
                    state = "GETMSG";
                }
                else if (t.await_io_completion.has_value())
                {
                    state = "IOCP-WAIT";
                }
                else if (t.await_time.has_value())
                {
                    constexpr auto infinite = std::chrono::steady_clock::time_point::min();
                    state = (t.await_time.value() == infinite) ? "SLEEP-inf" : "SLEEP-timed";
                }
                else
                {
                    state = "RUNNABLE";
                }

                if (t.apc_alertable)
                {
                    state += " alertable";
                }

                if (!t.pending_apcs.empty())
                {
                    bool any_io = false;
                    for (const auto& apc : t.pending_apcs)
                    {
                        any_io = any_io || apc.restamp_io_status_block;
                    }
                    state += " apc=" + std::to_string(t.pending_apcs.size());
                    if (any_io)
                    {
                        state += "(io)";
                    }
                }

                const auto* mod = mods.find_by_address(t.current_ip);
                win_emu.log.print(color::yellow, "  tid %u '%s' %s ip=%s+0x%llx\n", t.id, u16_to_u8(t.name).c_str(), state.c_str(),
                                  mod ? mod->name.c_str() : "?",
                                  mod ? static_cast<unsigned long long>(t.current_ip - mod->image_base)
                                      : static_cast<unsigned long long>(t.current_ip));
            }

            if (!win_emu.alert_trace.empty())
            {
                win_emu.log.print(color::yellow, "  -- recent alert/wait events (oldest first) --\n");
                for (const auto& event : win_emu.alert_trace)
                {
                    win_emu.log.print(color::yellow, "  %s\n", event.c_str());
                }
            }
        }

        bool switch_to_next_thread(windows_emulator& win_emu)
        {
            log_thread_stacks(win_emu);
            log_thread_waits(win_emu);
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
        std::unique_ptr<network::dns_lookup> get_dns_lookup(emulator_interfaces& interfaces)
        {
            if (interfaces.dns_lookup)
            {
                return std::move(interfaces.dns_lookup);
            }

            return std::make_unique<network::dns_lookup>();
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

        std::unique_ptr<ui_backend> get_ui_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.ui)
            {
                return std::move(interfaces.ui);
            }

            return create_default_ui_backend();
        }
    }

    windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, application_settings app_settings,
                                       const emulator_settings& settings, emulator_callbacks callbacks, emulator_interfaces interfaces)
        : windows_emulator(std::move(emu), settings, std::move(callbacks), std::move(interfaces))
    {
        fixup_application_settings(app_settings);
        this->application_settings_ = std::move(app_settings);
    }

    windows_emulator::windows_emulator(std::unique_ptr<x86_64_emulator> emu, const emulator_settings& settings,
                                       emulator_callbacks callbacks, emulator_interfaces interfaces)
        : emu_(std::move(emu)),
          clock_(get_clock(interfaces, this->executed_instructions_, settings.use_relative_time)),
          dns_lookup_(get_dns_lookup(interfaces)),
          socket_factory_(get_socket_factory(interfaces)),
          ui_backend_(get_ui_backend(interfaces)),
          emulation_root{settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root)},
          fake_env(settings.fake_env),
          callbacks(std::move(callbacks)),
          file_sys(emulation_root.empty() ? emulation_root : emulation_root / "filesys"),
          memory(*this->emu_),
          registry(emulation_root.empty() ? settings.registry_directory : emulation_root / "registry"),
          mod_manager(memory, file_sys, this->callbacks),
          process(*this->emu_, memory, *this->clock_, this->callbacks),
          use_relative_time_(settings.use_relative_time)
    {
        this->ui_backend_->set_event_sink([this](const ui_event& event) { this->handle_ui_event(event); });
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
        if (this->setup_completed_)
        {
            return;
        }

        this->setup_completed_ = true;

        this->setup_process();
    }

    void windows_emulator::setup_process()
    {
        const auto& emu = this->emu();
        auto& context = this->process;

        this->version.load_from_registry(this->registry, this->log);

        this->mod_manager.map_main_modules(this->application_settings_.application, this->version, context, this->log);
        this->install_section_first_execution_hooks();

        const auto* executable = this->mod_manager.executable;
        const auto* ntdll = this->mod_manager.ntdll;
        const auto* win32u = this->mod_manager.win32u;

        const auto apiset_data = apiset::obtain(this->emulation_root);

        this->process.setup(this->emu(), this->memory, this->registry, this->file_sys, this->version, this->fake_env,
                            this->application_settings_, *executable, *ntdll, apiset_data, this->mod_manager.wow64_modules_.ntdll32);

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

    void windows_emulator::trace_alert(std::string event)
    {
        static const bool enabled = std::getenv("EMULATOR_LOG_ALERTS") != nullptr || std::getenv("EMULATOR_LOG_EVENTS") != nullptr;
        if (!enabled)
        {
            return;
        }

        constexpr size_t max_events = 300;
        if (this->alert_trace.size() >= max_events)
        {
            this->alert_trace.pop_front();
        }
        this->alert_trace.push_back(std::move(event));
    }

    bool windows_emulator::perform_thread_switch()
    {
        const auto needed_switch = this->switch_thread_.exchange(false);

        this->switch_thread_ = false;
        while (!switch_to_next_thread(*this))
        {
            this->ui_backend_->pump_events();

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

        if (!this->uses_section_first_execution_hooks())
        {
            this->track_section_first_execution(address);
        }

        this->callbacks.on_instruction(address);
    }

    bool windows_emulator::uses_section_first_execution_hooks() const
    {
        return !this->emu().supports_global_memory_execution_hooks();
    }

    void windows_emulator::track_section_first_execution(const uint64_t address)
    {
        auto* mod = this->mod_manager.find_by_address(address);
        if (!mod)
        {
            return;
        }

        auto* hook_states = [&]() -> std::vector<emulator_hook*>* {
            const auto entry = this->section_first_execution_hooks_.find(mod->image_base);
            return entry == this->section_first_execution_hooks_.end() ? nullptr : &entry->second;
        }();

        for (size_t i = 0; i < mod->sections.size(); ++i)
        {
            auto& section = mod->sections[i];
            if (!is_within_start_and_length(address, section.region.start, section.region.length))
            {
                continue;
            }

            if (section.first_execute.has_value())
            {
                return;
            }

            section.first_execute = address;

            if (hook_states && i < hook_states->size() && (*hook_states)[i])
            {
                auto* hook = (*hook_states)[i];
                (*hook_states)[i] = nullptr;
                this->emu().delete_hook(hook);
            }

            this->callbacks.on_section_first_execution(*mod, section, address);

            return;
        }
    }

    void windows_emulator::clear_section_first_execution_hooks()
    {
        for (const auto& hooks : this->section_first_execution_hooks_ | std::views::values)
        {
            for (auto* hook : hooks)
            {
                if (hook)
                {
                    this->emu().delete_hook(hook);
                }
            }
        }

        this->section_first_execution_hooks_.clear();
    }

    void windows_emulator::install_section_first_execution_hook(const mapped_module& mod, const size_t section_index)
    {
        if (!this->uses_section_first_execution_hooks() || section_index >= mod.sections.size())
        {
            return;
        }

        const auto& section = mod.sections[section_index];
        if (section.first_execute.has_value() || section.region.length == 0)
        {
            return;
        }

        auto& hooks = this->section_first_execution_hooks_[mod.image_base];
        if (hooks.size() < mod.sections.size())
        {
            hooks.resize(mod.sections.size());
        }

        if (hooks[section_index])
        {
            return;
        }

        hooks[section_index] =
            this->emu().hook_memory_range_execution(section.region.start, section.region.length, [this](const uint64_t address) {
                this->track_section_first_execution(address); //
            });
    }

    void windows_emulator::install_section_first_execution_hooks()
    {
        if (!this->uses_section_first_execution_hooks())
        {
            return;
        }

        for (const auto& mod : this->mod_manager.modules() | std::views::values)
        {
            for (size_t i = 0; i < mod.sections.size(); ++i)
            {
                this->install_section_first_execution_hook(mod, i);
            }
        }
    }

    void windows_emulator::setup_hooks()
    {
        this->callbacks.on_module_load.add([this](mapped_module& mod) {
            for (size_t i = 0; i < mod.sections.size(); ++i)
            {
                this->install_section_first_execution_hook(mod, i);
            }
        });

        this->callbacks.on_module_unload.add([this](mapped_module& mod) {
            const auto hooks = this->section_first_execution_hooks_.extract(mod.image_base);
            if (hooks)
            {
                for (auto* hook : hooks.mapped())
                {
                    if (hook)
                    {
                        this->emu().delete_hook(hook);
                    }
                }
            }
        });

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
            case 41:
                this->callbacks.on_fast_fail(this->emu().reg<uint32_t>(x86_register::ecx));
                this->process.exit_status = STATUS_FAIL_FAST_EXCEPTION;
                this->stop();
                return;
            case 45:
                this->callbacks.on_suspicious_activity("DbgPrint");
                {
                    const auto cs_selector = this->emu().reg<uint16_t>(x86_register::cs);
                    const auto bitness = segment_utils::get_segment_bitness(this->emu(), cs_selector);
                    const auto service = this->emu().reg<uint32_t>(x86_register::eax);

                    if (bitness && *bitness == segment_utils::segment_bitness::bit64 &&
                        (service == BREAKPOINT_PRINT || service == BREAKPOINT_LOAD_SYMBOLS || service == BREAKPOINT_UNLOAD_SYMBOLS ||
                         service == BREAKPOINT_COMMAND_STRING))
                    {
                        const auto ip = this->emu().supports_instruction_counting() //
                                            ? this->current_thread().current_ip
                                            : this->emu().read_instruction_pointer();
                        this->emu().reg(x86_register::rip, ip + 3);
                    }
                    else
                    {
                        dispatch_breakpoint(*this);
                    }
                }
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
                    // A fault on a null/near-null address is almost always a call through a null function
                    // pointer (e.g. a Vulkan entry point the shim doesn't implement). Log the caller's
                    // return address so the missing function's call site can be identified.
                    if (address < 0x1000)
                    {
                        const auto sp = this->emu().reg<uint32_t>(x86_register::esp);
                        uint32_t return_address = 0;
                        if (this->emu().try_read_memory(sp, &return_address, sizeof(return_address)))
                        {
                            const auto* mod = this->mod_manager.find_by_address(return_address);
                            this->log.error("Null-pointer call to 0x%llx; caller return address 0x%x (%s+0x%llx)\n",
                                            static_cast<unsigned long long>(address), return_address, mod ? mod->name.c_str() : "?",
                                            mod ? static_cast<unsigned long long>(return_address - mod->image_base) : return_address);
                        }
                    }

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
        this->last_stop_reason_ = stop_reason::none;
        this->last_stop_detail_.clear();
        this->setup_process_if_necessary();

        const auto use_count = count > 0;
        const auto start_instructions = this->executed_instructions_;
        const auto target_instructions = start_instructions + count;

        std::mutex interrupt_mutex{};
        std::condition_variable interrupt_cond{};
        std::thread interrupt_thread{};

        const auto _ = utils::finally([&] {
            {
                std::unique_lock lock{interrupt_mutex};
                this->should_stop = true;
            }

            interrupt_cond.notify_all();

            if (interrupt_thread.joinable())
            {
                interrupt_thread.join();
            }
        });

        if (!this->emu().supports_instruction_counting())
        {
            interrupt_thread = std::thread([&] {
                while (!this->should_stop)
                {
                    std::unique_lock lock{interrupt_mutex};
                    interrupt_cond.wait_for(lock, std::chrono::seconds(1), [&] {
                        return this->should_stop.load(); //
                    });

                    if (!this->should_stop)
                    {
                        this->switch_thread_ = true;
                        this->emu().stop();
                    }
                }
            });
        }

        while (!this->should_stop)
        {
            this->ui_backend_->pump_events();
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

    void windows_emulator::handle_ui_event(const ui_event& event)
    {
        const auto* win = this->process.windows.get(event.window);
        if (!win)
        {
            return;
        }

        auto* thread = get_thread_by_id(this->process, win->thread_id);
        if (!thread)
        {
            return;
        }

        msg m{};
        m.window = event.window;
        m.message = event.message;
        m.wParam = event.wParam;
        m.lParam = event.lParam;

        if (is_pointer_message(event.message))
        {
            const auto target = route_pointer(this->process, event.window, point_x(event.lParam), point_y(event.lParam));
            m.window = target.window;
            m.lParam = pack_point(target.x, target.y);
        }

        thread->post_message(m);

        if (event.message == WM_CLOSE || event.message == WM_COMMAND || event.message == WM_KEYDOWN || event.message == WM_LBUTTONDOWN ||
            event.message == WM_LBUTTONUP)
        {
            this->switch_thread_ = true;
            this->emu().stop();
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
        buffer.write(this->application_settings_);
        buffer.write(this->setup_completed_);
        buffer.write(this->executed_instructions_);
        buffer.write_atomic(this->switch_thread_);
        buffer.write(this->use_relative_time_);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Backend snapshot mode is not used here; Unicorn's in-place snapshot path is broken.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer);
    }

    void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
    {
        this->register_factories(buffer);

        buffer.read(this->application_settings_);
        buffer.read(this->setup_completed_);
        buffer.read(this->executed_instructions_);
        buffer.read_atomic(this->switch_thread_);

        const auto old_relative_time = this->use_relative_time_;
        buffer.read(this->use_relative_time_);

        if (old_relative_time != this->use_relative_time_)
        {
            throw std::runtime_error("Can not deserialize emulator with different time dimensions");
        }

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();

        // Match raw serialize() above; do not use backend snapshot mode here.
        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        this->process.deserialize(buffer);
    }

    void windows_emulator::save_snapshot()
    {
        utils::buffer_serializer buffer{};

        buffer.write(this->setup_completed_);
        buffer.write(this->executed_instructions_);
        buffer.write_atomic(this->switch_thread_);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Snapshot path still uses regular backend state serialization.
        // Backend snapshot mode (is_snapshot=true) is not reliable yet.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer);

        this->process_snapshot_ = buffer.move_buffer();
    }

    void windows_emulator::restore_snapshot()
    {
        if (this->process_snapshot_.empty())
        {
            throw std::runtime_error("No snapshot saved");
        }

        utils::buffer_deserializer buffer{this->process_snapshot_};

        this->register_factories(buffer);

        buffer.read(this->setup_completed_);
        buffer.read(this->executed_instructions_);
        buffer.read_atomic(this->switch_thread_);

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();

        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        this->process.deserialize(buffer);
    }

} // namespace sogen
