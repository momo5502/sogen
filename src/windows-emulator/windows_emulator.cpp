#include "std_include.hpp"
#include "windows_emulator.hpp"
#include "keyboard_layout.hpp"

#include "cpu_context.hpp"

#include <utils/io.hpp>
#include <utils/timer.hpp>
#include <utils/finally.hpp>
#include <utils/lazy_object.hpp>
#include <utils/string.hpp>

#include "exception_dispatch.hpp"
#include "apiset/apiset.hpp"
#include "syscall_dispatcher.hpp"

#include "memory_permission_ext.hpp"

namespace sogen
{
    constexpr auto MAX_INSTRUCTIONS_PER_TIME_SLICE = 0x20000;
    constexpr auto MAX_BASIC_BLOCKS_PER_TIME_SLICE = 0x8000;

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

        uint16_t high_word(const uint64_t value)
        {
            return static_cast<uint16_t>((value >> 16) & 0xFFFFu);
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

        std::optional<POINT> get_client_origin_relative_to_ancestor(const process_context& process, const hwnd window, const hwnd ancestor)
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

                origin.x += current->client_x();
                origin.y += current->client_y();
                current_handle = current->parent_handle;
            }

            if (current_handle != ancestor)
            {
                return std::nullopt;
            }

            return origin;
        }

        bool is_mouse_wheel_message(const uint32_t message)
        {
            return message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL;
        }

        bool is_mouse_button_message(const uint32_t message)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return true;
            default:
                return false;
            }
        }

        bool is_mouse_button_down_message(const uint32_t message)
        {
            return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
        }

        bool is_mouse_button_up_message(const uint32_t message)
        {
            return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
        }

        bool is_pointer_message(const uint32_t message)
        {
            // All mouse messages go through capture/child hit-testing: while a window holds the mouse
            // capture every mouse message must reach it (so a pressed button still completes its click),
            // and otherwise each is delivered to the child under the cursor (hover, right-click, etc.).
            return message == WM_MOUSEMOVE || is_mouse_button_message(message) || is_mouse_wheel_message(message);
        }

        bool is_key_down_message(const uint32_t message)
        {
            return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        }

        bool is_key_up_message(const uint32_t message)
        {
            return message == WM_KEYUP || message == WM_SYSKEYUP;
        }

        constexpr int k_wh_keyboard_ll = 13;
        constexpr int k_wh_mouse_ll = 14;

        bool is_character_message(const uint32_t message)
        {
            return message == WM_CHAR || message == WM_SYSCHAR || message == WM_DEADCHAR || message == WM_SYSDEADCHAR;
        }

        int low_level_hook_id(const uint32_t message)
        {
            switch (message)
            {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                return k_wh_keyboard_ll;
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return k_wh_mouse_ll;
            default:
                return 0;
            }
        }

        bool is_input_message(const uint32_t message)
        {
            return low_level_hook_id(message) != 0 || is_character_message(message) || is_pointer_message(message);
        }

        bool is_keyboard_message(const uint32_t message)
        {
            return is_key_down_message(message) || is_key_up_message(message);
        }

        // Window button message -> RAWMOUSE usButtonFlags transition bit (winuser.h RI_MOUSE_* values).
        uint16_t raw_mouse_button_flags(const uint32_t message, const uint64_t wparam)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
                return RI_MOUSE_LEFT_BUTTON_DOWN;
            case WM_LBUTTONUP:
                return RI_MOUSE_LEFT_BUTTON_UP;
            case WM_RBUTTONDOWN:
                return RI_MOUSE_RIGHT_BUTTON_DOWN;
            case WM_RBUTTONUP:
                return RI_MOUSE_RIGHT_BUTTON_UP;
            case WM_MBUTTONDOWN:
                return RI_MOUSE_MIDDLE_BUTTON_DOWN;
            case WM_MBUTTONUP:
                return RI_MOUSE_MIDDLE_BUTTON_UP;
            case WM_XBUTTONDOWN:
                return high_word(wparam) == XBUTTON2 ? RI_MOUSE_BUTTON_5_DOWN : RI_MOUSE_BUTTON_4_DOWN;
            case WM_XBUTTONUP:
                return high_word(wparam) == XBUTTON2 ? RI_MOUSE_BUTTON_5_UP : RI_MOUSE_BUTTON_4_UP;
            case WM_MOUSEWHEEL:
                return RI_MOUSE_WHEEL;
            case WM_MOUSEHWHEEL:
                return RI_MOUSE_HWHEEL;
            default:
                return 0;
            }
        }

        uint16_t raw_mouse_button_data(const uint32_t message, const uint64_t wparam)
        {
            if (is_mouse_wheel_message(message))
            {
                return high_word(wparam);
            }
            return 0;
        }

        uint8_t mouse_button_virtual_key(const uint32_t message, const uint64_t wparam)
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                return VK_LBUTTON;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
                return VK_RBUTTON;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                return VK_MBUTTON;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                return high_word(wparam) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
            default:
                return 0;
            }
        }

        // Best-effort US-layout virtual-key -> PS/2 set-1 scan code for the RAWKEYBOARD MakeCode field
        // (games that bind by scan code need it; the VKey is delivered too for those that use it).
        uint16_t vk_to_scan_code(const uint16_t vk)
        {
            switch (vk)
            {
            case VK_ESCAPE:
                return 0x01;
            case VK_RETURN:
                return 0x1C;
            case VK_SPACE:
                return 0x39;
            case VK_TAB:
                return 0x0F;
            case VK_BACK:
                return 0x0E;
            case VK_SHIFT:
                return 0x2A;
            case VK_CONTROL:
                return 0x1D;
            case VK_MENU:
                return 0x38;
            case VK_UP:
                return 0x48;
            case VK_DOWN:
                return 0x50;
            case VK_LEFT:
                return 0x4B;
            case VK_RIGHT:
                return 0x4D;
            default:
                if (vk >= 'A' && vk <= 'Z')
                {
                    static constexpr std::array<uint8_t, 26> letter_scan = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
                                                                            0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
                                                                            0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
                    return letter_scan[static_cast<size_t>(vk - 'A')];
                }
                if (vk >= '1' && vk <= '9')
                {
                    return static_cast<uint16_t>(0x02 + (vk - '1'));
                }
                if (vk == '0')
                {
                    return 0x0B;
                }
                return 0;
            }
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
                    const auto captured_origin = get_client_origin_relative_to_ancestor(process, captured->handle, 0);
                    const auto top_level_origin = get_client_origin_relative_to_ancestor(process, top_level, 0);
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

        bool has_pending_host_wait(const process_context& process)
        {
            for (const auto& thread_entry : process.threads)
            {
                if (thread_entry.second.await_host_condition)
                {
                    return true;
                }
            }

            return false;
        }

        void perform_context_switch_work(windows_emulator& win_emu, vcpu_context& vcpu)
        {
            win_emu.apply_pending_guest_event_signals();

            auto& threads = win_emu.process.threads;

            for (auto it = threads.begin(); it != threads.end();)
            {
                auto& thread = it->second;
                if (!thread.can_be_deleted(&vcpu))
                {
                    ++it;
                    continue;
                }

                if (vcpu.active_thread == &thread)
                {
                    vcpu.active_thread = nullptr;
                }

                it = threads.erase(it).first;
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

        void dispatch_next_apc(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread)
        {
            assert(vcpu.active_thread == &thread);

            auto& apcs = thread.pending_apcs;
            if (apcs.empty())
            {
                return;
            }

            thread.setup_if_necessary(vcpu.cpu, win_emu.process);
            auto& cpu = vcpu.cpu;

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
                cpu.write_memory(next_apx.apc_argument2, &status32, sizeof(status32));
                cpu.write_memory(next_apx.apc_argument2 + sizeof(status32), &next_apx.io_information, sizeof(next_apx.io_information));
            }

            if (auto* arm64 = dynamic_cast<arm64_cpu*>(&cpu))
            {
                // KiUserApcDispatcher frame: routine, three arguments, flags (bit 0: test alert), then CONTEXT at +0x30.
                struct
                {
                    uint64_t routine{};
                    std::array<uint64_t, 3> arguments{};
                    uint32_t flags{};
                    uint32_t reserved{};
                    uint64_t padding{};
                    arm64_context context{};
                } arm64_layout;

                static_assert(offsetof(decltype(arm64_layout), context) == 0x30);

                arm64_layout.routine = next_apx.apc_routine;
                arm64_layout.arguments = {next_apx.apc_argument1, next_apx.apc_argument2, next_apx.apc_argument3};
                arm64_layout.flags = 1;
                arm64_layout.context.ContextFlags = arm64_context_all;
                cpu_context::save(*arm64, arm64_layout.context);

                const auto new_sp = align_down(arm64->read_stack_pointer() - sizeof(arm64_layout), 0x10);
                arm64->write_memory(new_sp, arm64_layout);
                arm64->reg(arm64_register::sp, new_sp);
                arm64->reg(arm64_register::pc, win_emu.process.ki_user_apc_dispatcher);
                return;
            }

            auto& emu = vcpu.x64_cpu();

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

        vcpu_context* find_vcpu_running_thread(windows_emulator& win_emu, const emulator_thread& thread)
        {
            for (uint32_t i = 0; i < win_emu.vcpu_count(); ++i)
            {
                auto& vcpu = win_emu.vcpu(i);
                if (vcpu.active_thread == &thread)
                {
                    return &vcpu;
                }
            }

            return nullptr;
        }

        bool switch_to_thread(windows_emulator& win_emu, vcpu_context& vcpu, emulator_thread& thread, const bool force = false)
        {
            if (thread.is_terminated())
            {
                return false;
            }

            // A thread that is loaded on another vCPU can only run there.
            if (auto* running_on = find_vcpu_running_thread(win_emu, thread); running_on && running_on != &vcpu)
            {
                return false;
            }

            if (thread.registers_in_transit || thread.pending_register_accesses != 0)
            {
                return false;
            }

            auto& emu = vcpu.cpu;
            auto& context = win_emu.process;

            const auto is_ready = thread.is_thread_ready(win_emu);
            const auto has_pending_status = thread.pending_status.has_value();
            const auto can_dispatch_apcs = thread.apc_alertable && !thread.pending_apcs.empty();
            const auto can_run_low_level_hook =
                !is_ready && thread.message_wait_return_register.has_value() && win_emu.has_low_level_hook_work(thread.id);

            if (!is_ready && !force && !can_dispatch_apcs && !can_run_low_level_hook)
            {
                return false;
            }

            auto* active_thread = vcpu.active_thread;

            if (active_thread != &thread)
            {
                if (active_thread)
                {
                    win_emu.callbacks.on_thread_switch(*active_thread, thread);
                }

                vcpu.set_active_thread(&thread);

                win_emu.transfer_thread_registers(vcpu, active_thread, thread);
            }

            thread.setup_if_necessary(emu, context);

            if (can_dispatch_apcs && !has_pending_status)
            {
                thread.mark_as_ready(STATUS_USER_APC);
                dispatch_next_apc(win_emu, vcpu, thread);
            }
            else if (can_run_low_level_hook)
            {
                // The message syscall runs the hook first when it executes again.
                thread.restart_message_wait(emu, win_emu.abi());
            }

            thread.apc_alertable = false;
            return true;
        }

        bool switch_to_thread(windows_emulator& win_emu, vcpu_context& vcpu, const handle thread_handle)
        {
            auto* thread = win_emu.process.threads.get(thread_handle);
            if (!thread)
            {
                throw std::runtime_error("Bad thread handle");
            }

            return switch_to_thread(win_emu, vcpu, *thread);
        }

        bool switch_to_next_thread(windows_emulator& win_emu, vcpu_context& vcpu)
        {
            const scheduler_clock::scope time_scope{win_emu.scheduling_clock()};
            perform_context_switch_work(win_emu, vcpu);

            auto& threads = win_emu.process.threads;

            auto resume_at = vcpu.active_thread ? threads.find(*vcpu.active_thread) : threads.end();
            if (resume_at != threads.end())
            {
                ++resume_at;
            }

            for (auto it = resume_at; it != threads.end(); ++it)
            {
                if (switch_to_thread(win_emu, vcpu, it->second))
                {
                    return true;
                }
            }

            for (auto it = threads.begin(); it != resume_at; ++it)
            {
                if (switch_to_thread(win_emu, vcpu, it->second))
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

        std::unique_ptr<network::dns_lookup> get_dns_lookup(emulator_interfaces& interfaces, const network::network_mode mode)
        {
            if (interfaces.dns_lookup)
            {
                return std::move(interfaces.dns_lookup);
            }

            return network::create_dns_lookup(mode);
        }

        std::unique_ptr<network::socket_factory> get_socket_factory(emulator_interfaces& interfaces, const network::network_mode mode)
        {
            if (interfaces.socket_factory)
            {
                return std::move(interfaces.socket_factory);
            }

            return network::create_socket_factory(mode);
        }

        std::unique_ptr<ui_backend> get_ui_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.ui)
            {
                return std::move(interfaces.ui);
            }

            return create_default_ui_backend();
        }

        std::unique_ptr<audio_backend> get_audio_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.audio)
            {
                return std::move(interfaces.audio);
            }

            return create_default_audio_backend();
        }

        std::unique_ptr<crypt_protect_backend> get_crypt_protect_backend(emulator_interfaces& interfaces,
                                                                         const std::filesystem::path& emulation_root)
        {
            if (interfaces.crypt_protect)
            {
                return std::move(interfaces.crypt_protect);
            }

            return create_default_crypt_protect_backend(emulation_root);
        }

        std::unique_ptr<console_backend> get_console_backend(emulator_interfaces& interfaces)
        {
            if (interfaces.console)
            {
                return std::move(interfaces.console);
            }

            return create_default_console_backend();
        }

        // The guest must see at least as many logical processors as there are vCPUs, otherwise a
        // thread running on a higher-indexed vCPU would report a processor number the guest
        // considers out of range. The configured fake value still wins when it is larger (e.g. the
        // anti-analysis default of 4 with a single vCPU).
        fake_environment_config effective_fake_env(const emulator_settings& settings, const uint32_t vcpu_count)
        {
            auto fake_env = settings.fake_env;
            fake_env.number_of_processors = std::max(fake_env.number_of_processors, vcpu_count);
            return fake_env;
        }
    }

    PEMachineType get_application_machine(const emulator_settings& settings, application_settings app_settings)
    {
        fixup_application_settings(app_settings);
        const auto& application = app_settings.application;

        const auto root = settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root);
        file_system file_sys{root.empty() ? root : root / "filesys", settings.root_mode};
        for (const auto& mount : settings.mounts)
        {
            file_sys.map(mount.guest_path, mount.host_path, mount.mode);
        }

        const auto detection = pe_architecture_detector::detect_from_file(file_sys, application);
        if (!detection.is_valid())
        {
            throw std::runtime_error("Cannot determine the machine type of " + application.string() + ": " + detection.error_message);
        }

        return detection.machine;
    }

    machine_type get_machine_type(const PEMachineType machine)
    {
        switch (machine)
        {
        case PEMachineType::AMD64:
        case PEMachineType::ARM64EC:
        case PEMachineType::I386:
            return machine_type::x86_64;
        case PEMachineType::ARM64:
            return machine_type::arm64;
        default:
            throw std::runtime_error("Unsupported executable machine type: 0x" +
                                     utils::string::to_hex_number(static_cast<uint16_t>(machine)));
        }
    }

    windows_emulator::windows_emulator(std::unique_ptr<guest_machine> emu, application_settings app_settings,
                                       const emulator_settings& settings, emulator_callbacks callbacks, emulator_interfaces interfaces)
        : windows_emulator(std::move(emu), settings, std::move(callbacks), std::move(interfaces))
    {
        fixup_application_settings(app_settings);
        this->application_settings_ = std::move(app_settings);
    }

    windows_emulator::windows_emulator(std::unique_ptr<guest_machine> emu, const emulator_settings& settings, emulator_callbacks callbacks,
                                       emulator_interfaces interfaces)
        : emu_(std::move(emu)),
          use_native_timestamp_counter_(!interfaces.clock && !settings.use_relative_time &&
                                        this->emu_->supports_native_timestamp_counter()),
          clock_(std::make_unique<scheduler_clock>(get_clock(interfaces, this->executed_instructions_, settings.use_relative_time))),
          network_mode_(settings.network),
          dns_lookup_(get_dns_lookup(interfaces, settings.network)),
          socket_factory_(get_socket_factory(interfaces, settings.network)),
          ui_backend_(get_ui_backend(interfaces)),
          audio_backend_(get_audio_backend(interfaces)),
          crypt_protect_backend_(get_crypt_protect_backend(
              interfaces, settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root))),
          process_manager_(interfaces.processes),
          console_backend_(get_console_backend(interfaces)),
          emulation_root{settings.emulation_root.empty() ? settings.emulation_root : absolute(settings.emulation_root)},
          fake_env(effective_fake_env(settings, static_cast<uint32_t>(this->emu_->vcpu_count()))),
          callbacks(std::move(callbacks)),
          file_sys(emulation_root.empty() ? emulation_root : emulation_root / "filesys", settings.root_mode),
          memory(*this->emu_),
          registry(settings.load_registry
                       ? registry_manager{emulation_root.empty() ? settings.registry_directory : emulation_root / "registry"}
                       : registry_manager{}),
          mod_manager(memory, file_sys, this->callbacks, guest_architecture_of(*this->emu_)),
          process(*this->emu_, memory, *this->clock_, this->callbacks, settings.intercept_kusd, guest_architecture_of(*this->emu_)),
          use_relative_time_(settings.use_relative_time),
          // Instruction precision runs through a global execution hook; backends that only count instructions
          // (single-stepping) cannot provide it.
          instruction_precision_(settings.use_instruction_precision && this->emu_->get_guest_cpu(0).supports_instruction_counting() &&
                                 this->emu_->supports_global_memory_execution_hooks()),
          intercept_rdtsc_(settings.intercept_rdtsc),
          intercept_kusd_(settings.intercept_kusd),
          spoof_gpu_features_(settings.spoof_gpu_features),
          elevated_(settings.elevated),
          vcpu_count_(static_cast<uint32_t>(this->emu_->vcpu_count()))
    {
        if (this->vcpu_count_ == 0)
        {
            throw std::invalid_argument("At least one vCPU is required");
        }

        if (this->vcpu_count_ > 1 && !this->emu_->supports_multiple_vcpus())
        {
            throw std::invalid_argument("The " + this->emu_->get_name() + " backend does not support multiple vCPUs");
        }

        if (this->vcpu_count_ > 1)
        {
            // Deliberate hard errors instead of silent clamping (docs/multi-vcpu-design.md).
            if (this->instruction_precision_)
            {
                throw std::invalid_argument("Instruction precision requires a single vCPU");
            }

            if (this->use_relative_time_)
            {
                throw std::invalid_argument("Relative time requires a single vCPU");
            }
        }

        this->vcpus_.reserve(this->vcpu_count_);
        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
        {
            this->vcpus_.push_back(std::make_unique<vcpu_context>(this->emu_->get_guest_cpu(i)));
        }

        this->ui_backend_->set_event_sink([this](const ui_event& event) { this->handle_ui_event(event); });
#ifndef OS_WINDOWS
        if (this->emulation_root.empty())
        {
            throw std::runtime_error("Emulation root directory can not be empty!");
        }
#endif

        for (const auto& mount : settings.mounts)
        {
            this->file_sys.map(mount.guest_path, mount.host_path, mount.mode);
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

        this->mod_manager.map_main_modules(this->vcpu(0).cpu, this->application_settings_.application, this->version, context, this->log);
        this->install_section_first_execution_hooks();

        const auto* executable = this->mod_manager.executable;
        const auto* ntdll = this->mod_manager.ntdll;
        const auto* win32u = this->mod_manager.win32u;

        const auto apiset_data =
            apiset::obtain(this->file_sys.read_file(this->version.get_system_root() / "System32" / "apisetschema.dll"));

        this->process.setup(*this, this->application_settings_, *executable, *ntdll, apiset_data, this->mod_manager.wow64_modules_.ntdll32);

        const auto ntdll_data = emu.read_memory(ntdll->image_base, static_cast<size_t>(ntdll->size_of_image));
        const auto win32u_data = emu.read_memory(win32u->image_base, static_cast<size_t>(win32u->size_of_image));

        this->dispatcher.setup(ntdll->exports, ntdll_data, win32u->exports, win32u_data, this->abi().syscall_abi());

        const auto initial_argument = this->process.is_wow64_process ? this->process.peb32->value() : this->process.peb64.value();
        const auto main_thread_id = context.create_thread(this->memory, this->mod_manager.executable->entry_point, initial_argument,
                                                          this->mod_manager.executable->size_of_stack_reserve, 0, true);

        const std::scoped_lock lock(this->kernel_lock_);
        switch_to_thread(*this, this->vcpu(0), main_thread_id);
    }

    void windows_emulator::yield_thread(vcpu_context& vcpu, const bool alertable)
    {
        this->kernel_lock_.assert_held();

        vcpu.switch_thread = true;
        vcpu.thread().apc_alertable = alertable;
        vcpu.cpu.stop();
    }

    bool windows_emulator::perform_thread_switch(vcpu_context& vcpu)
    {
        std::unique_lock lock(this->kernel_lock_);
        return this->perform_thread_switch(vcpu, lock);
    }

    bool windows_emulator::perform_thread_switch(vcpu_context& vcpu, std::unique_lock<kernel_lock>& lock)
    {
        this->kernel_lock_.assert_held();

        const auto needed_switch = vcpu.switch_thread.exchange(false);
        bool woken_by_signal = false;

        while (!switch_to_next_thread(*this, vcpu))
        {
            woken_by_signal = false;

            if (this->vcpu_count_ > 1 && vcpu.active_thread)
            {
                // Nothing runnable for this vCPU: detach the stale thread so another
                // vCPU can pick it up once it becomes ready.
                vcpu.active_thread->save(vcpu.cpu);
                vcpu.active_thread = nullptr;
                this->thread_registers_saved_.notify_all();
            }

            const auto host_wait_pending = has_pending_host_wait(this->process);

            ++this->idle_vcpus_;

            // Idle: nothing is ready. Release the kernel lock while pumping UI events
            // and sleeping so other threads (UI delivery, vCPU workers) can run.
            lock.unlock();

            if (this->vcpu_count_ == 1)
            {
                // With workers, the thread calling start() pumps UI events instead.
                this->ui_backend_->pump_events();
            }

            if (this->use_relative_time_)
            {
                this->executed_instructions_ += MAX_INSTRUCTIONS_PER_TIME_SLICE;
            }
            else if (host_wait_pending)
            {
                // A host wait (e.g. a GPU semaphore) is parked - re-poll immediately to wake it promptly.
                std::this_thread::yield();
            }
            else
            {
                woken_by_signal = this->wait_for_idle_wakeup(std::chrono::milliseconds(1));
            }

            --this->idle_vcpus_;
            lock.lock();

            this->drain_low_level_input();

            if (this->should_stop)
            {
                vcpu.switch_thread = needed_switch;
                return false;
            }
        }

        if (woken_by_signal)
        {
            this->wake_idle_vcpu();
        }

        return true;
    }

    void windows_emulator::vcpu_worker(vcpu_context& vcpu)
    {
        std::unique_lock lock(this->kernel_lock_);

        while (!this->should_stop)
        {
            // A loop rather than a single check: the register transfer of a switch releases the kernel lock, so the
            // chosen thread can be terminated or suspended before it runs.
            bool switched = true;
            while (vcpu.switch_thread || !vcpu.active_thread || !vcpu.thread().is_thread_ready(*this))
            {
                switched = this->perform_thread_switch(vcpu, lock);
                if (!switched)
                {
                    break;
                }
            }

            if (!switched || this->should_stop)
            {
                break;
            }

            // Guest code executes with the kernel lock released; hook callbacks
            // (syscalls, exceptions, exec hooks) re-acquire it on VM exit.
            lock.unlock();
            vcpu.cpu.start();
            lock.lock();

            if (!vcpu.switch_thread && !vcpu.cpu.has_violation())
            {
                break;
            }
        }

        lock.unlock();

        // One vCPU winding down (process exit, fatal error) ends the whole run.
        this->stop();
    }

    void windows_emulator::transfer_thread_registers(vcpu_context& vcpu, emulator_thread* outgoing, emulator_thread& incoming)
    {
        this->kernel_lock_.assert_held();

        if (this->vcpu_count_ == 1)
        {
            if (outgoing)
            {
                outgoing->save(vcpu.cpu);
            }

            incoming.restore(vcpu.cpu, this->process.gdt_base);
            return;
        }

        if (outgoing)
        {
            outgoing->registers_in_transit = true;
        }
        incoming.registers_in_transit = true;

        {
            this->kernel_lock_.unlock();
            const auto _ = utils::finally([&] {
                this->kernel_lock_.lock();
                if (outgoing)
                {
                    outgoing->registers_in_transit = false;
                }
                incoming.registers_in_transit = false;
                this->thread_registers_saved_.notify_all();
            });

            if (outgoing)
            {
                outgoing->save(vcpu.cpu);
            }

            incoming.load_registers(vcpu.cpu);
        }

        incoming.refresh_execution_context(vcpu.cpu, this->process.gdt_base);
    }

    bool windows_emulator::wait_for_thread_registers(vcpu_context& vcpu, emulator_thread& thread)
    {
        this->kernel_lock_.assert_held();

        const auto loaded_elsewhere = [&]() -> vcpu_context* {
            auto* owner = find_vcpu_running_thread(*this, thread);
            return owner && owner != &vcpu && !owner->active_thread_saved ? owner : nullptr;
        };

        if (!thread.registers_in_transit && !loaded_elsewhere())
        {
            return true;
        }

        const auto was_saved = std::exchange(vcpu.active_thread_saved, true);
        ++thread.pending_register_accesses;
        this->thread_registers_saved_.notify_all();
        const auto _ = utils::finally([&] {
            vcpu.active_thread_saved = was_saved;
            --thread.pending_register_accesses;
            this->wake_idle_vcpu();
        });

        while (thread.registers_in_transit || loaded_elsewhere())
        {
            if (this->should_stop)
            {
                return false;
            }

            if (auto* owner = thread.registers_in_transit ? nullptr : loaded_elsewhere())
            {
                owner->switch_thread = true;
                owner->cpu.stop();
            }

            // Bounded, so that the kick is repeated: a stop that reaches a vCPU just before it enters guest code is
            // lost on WHP.
            auto* dispatch_vcpu = std::exchange(this->dispatch_vcpu_, nullptr);
            this->thread_registers_saved_.wait_for(this->kernel_lock_, std::chrono::milliseconds(1));
            this->dispatch_vcpu_ = dispatch_vcpu;
        }

        return true;
    }

    bool windows_emulator::save_dispatching_thread(vcpu_context* vcpu)
    {
        if (this->vcpu_count_ == 1 || !vcpu || !vcpu->active_thread || vcpu->active_thread_saved)
        {
            return false;
        }

        vcpu->active_thread->save(vcpu->cpu);
        vcpu->active_thread_saved = true;
        this->thread_registers_saved_.notify_all();
        return true;
    }

    void windows_emulator::reload_dispatching_thread(vcpu_context& vcpu) const
    {
        vcpu.thread().restore(vcpu.cpu, this->process.gdt_base);
    }

    bool windows_emulator::activate_thread(vcpu_context& vcpu, const uint32_t id)
    {
        const std::scoped_lock lock(this->kernel_lock_);

        auto* thread = get_thread_by_id(this->process, id);
        if (!thread)
        {
            return false;
        }

        return switch_to_thread(*this, vcpu, *thread, true);
    }

    void windows_emulator::on_instruction_execution(vcpu_context& vcpu, const uint64_t address)
    {
        auto& thread = vcpu.thread();

        if (!thread.callback_stack.empty() && address == this->process.zw_callback_return)
        {
            thread.callback_return_rax = this->abi().return_value(vcpu.cpu);
        }

        ++this->executed_instructions_;
        const auto thread_insts = ++thread.executed_instructions;
        if (thread_insts % MAX_INSTRUCTIONS_PER_TIME_SLICE == 0)
        {
            this->yield_thread(vcpu);
        }

        thread.previous_ip = thread.current_ip;
        thread.current_ip = vcpu.cpu.read_instruction_pointer();

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

        hooks[section_index] = this->emu().hook_memory_range_execution(section.region.start, section.region.length,
                                                                       [this](cpu_interface&, const uint64_t address) {
                                                                           const std::scoped_lock lock(this->kernel_lock_);
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

        if (auto* x64 = dynamic_cast<x86_64_emulator*>(this->emu_.get()))
        {
            this->setup_x64_hooks(*x64);
        }
        else if (auto* arm64 = dynamic_cast<arm64_emulator*>(this->emu_.get()))
        {
            this->setup_arm64_hooks(*arm64);
        }
        else
        {
            throw std::runtime_error("Unsupported Windows guest machine: " + this->emu().get_name());
        }

        this->emu().hook_memory_violation([&](cpu_interface& cpu, const uint64_t address, const size_t size,
                                              const memory_operation operation, const memory_violation_type type) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto* x64 = dynamic_cast<x86_64_cpu*>(&vcpu.cpu);
            if (x64 && x64->reg<uint16_t>(x86_register::cs) == 0x33)
            {
                // loading gs selector only works in 64-bit mode
                const auto required_gs_base = vcpu.thread().gs_segment->get_base();
                const auto actual_gs_base = x64->get_segment_base(x86_register::gs);
                if (actual_gs_base != required_gs_base)
                {
                    x64->set_segment_base(x86_register::gs, required_gs_base);
                    return memory_violation_continuation::restart;
                }
            }

            auto region = this->memory.get_region_info(address);
            if (region.permissions.is_guarded())
            {
                // Unset the GUARD_PAGE flag and dispatch a STATUS_GUARD_PAGE_VIOLATION
                this->memory.protect_memory(region.allocation_base, region.length, region.permissions & ~memory_permission_ext::guard);
                dispatch_guard_page_violation(*this, vcpu, address, operation);
            }
            else
            {
                // A fault on a null/near-null address is almost always a call through a null function
                // pointer (e.g. a Vulkan entry point the shim doesn't implement). Log the caller's
                // return address so the missing function's call site can be identified.
                if (x64 && address < 0x1000)
                {
                    const auto sp = x64->reg<uint32_t>(x86_register::esp);
                    uint32_t return_address = 0;
                    if (x64->try_read_memory(sp, &return_address, sizeof(return_address)))
                    {
                        const auto* mod = this->mod_manager.find_by_address(return_address);
                        this->log.error("Null-pointer call to 0x%llx; caller return address 0x%x (%s+0x%llx)\n",
                                        static_cast<unsigned long long>(address), return_address, mod ? mod->name.c_str() : "?",
                                        mod ? static_cast<unsigned long long>(return_address - mod->image_base) : return_address);
                    }
                }

                this->callbacks.on_memory_violate(address, size, operation, type);
                dispatch_access_violation(*this, vcpu, address, operation);
            }

            return memory_violation_continuation::resume;
        });

        if (this->uses_instruction_precision())
        {
            this->emu().hook_memory_execution([&](cpu_interface& cpu, const uint64_t address) {
                const std::scoped_lock lock(this->kernel_lock_);
                auto& vcpu = this->vcpu(cpu.index());
                const scoped_dispatch dispatch(*this, vcpu);
                this->on_instruction_execution(vcpu, address); //
            });
        }
        else if (!this->emu().get_guest_cpu(0).is_stop_thread_safe())
        {
            // The backend cannot be stopped safely from another thread, so the interrupt thread in start()
            // is not available for time-slicing. Preempt cooperatively from the CPU thread via a basic-block
            // hook instead.
            this->emu().hook_basic_block([&](cpu_interface& cpu, const basic_block& block) {
                const std::scoped_lock lock(this->kernel_lock_);
                this->on_basic_block_execution(this->vcpu(cpu.index()), block); //
            });
        }
    }

    void windows_emulator::setup_arm64_hooks(arm64_emulator& emu)
    {
        // Handlers that move PC rewind it by the SVC size, so skipping the SVC lands on their target.
        emu.hook_instruction(arm64_hookable_instructions::svc, [&](cpu_interface& cpu, uint64_t) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            this->dispatcher.dispatch(*this, vcpu);
            return instruction_hook_continuation::skip_instruction;
        });

        emu.hook_instruction(arm64_hookable_instructions::read_counter, [&](cpu_interface& cpu, const uint64_t destination) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            this->callbacks.on_rdtsc();

            const auto ticks = this->clock_->timestamp_counter();
            dynamic_cast<arm64_cpu&>(vcpu.cpu).reg(static_cast<arm64_register>(destination), ticks);
            return instruction_hook_continuation::skip_instruction;
        });

        emu.hook_interrupt([&](cpu_interface& cpu, const int exception_class) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            this->callbacks.on_exception();

            switch (static_cast<arm64_exception_class>(exception_class))
            {
            case arm64_exception_class::undefined_instruction:
                this->callbacks.on_suspicious_activity("Illegal instruction");
                dispatch_illegal_instruction_violation(*this, vcpu);
                return;
            case arm64_exception_class::breakpoint_instruction:
                this->handle_arm64_breakpoint(vcpu);
                return;
            case arm64_exception_class::software_step:
                // The step completed, so it is reported at the next instruction.
                vcpu.thread().current_ip = vcpu.cpu.read_instruction_pointer();
                dispatch_single_step(*this, vcpu);
                return;
            default:
                throw std::runtime_error("Unsupported ARM64 guest exception class " + std::to_string(exception_class));
            }
        });
    }

    // Windows on ARM selects the kernel action by the BRK immediate; unassigned immediates are illegal instructions.
    void windows_emulator::handle_arm64_breakpoint(vcpu_context& vcpu)
    {
        auto& cpu = dynamic_cast<arm64_cpu&>(vcpu.cpu);
        const auto pc = cpu.read_instruction_pointer();

        switch ((cpu.read_memory<uint32_t>(pc) >> 5) & 0xFFFF)
        {
        case 0xF000:
            this->callbacks.on_suspicious_activity("Breakpoint");
            dispatch_breakpoint(*this, vcpu);
            return;
        case 0xF001:
            dispatch_exception(*this, vcpu, STATUS_ASSERTION_FAILURE, {});
            return;
        case 0xF002: {
            this->callbacks.on_suspicious_activity("DbgPrint");
            const auto service = cpu.reg(arm64_register::x16);
            if (service == BREAKPOINT_PRINT || service == BREAKPOINT_LOAD_SYMBOLS || service == BREAKPOINT_UNLOAD_SYMBOLS ||
                service == BREAKPOINT_COMMAND_STRING)
            {
                // ntdll follows the debug service BRK with BRK #0xF000 for an attached debugger to stop on.
                cpu.reg(arm64_register::pc, pc + 8);
            }
            else
            {
                dispatch_breakpoint(*this, vcpu);
            }
            return;
        }
        case 0xF003:
            this->callbacks.on_fast_fail(cpu.reg<uint32_t>(arm64_register::w0));
            this->process.exit_status = STATUS_FAIL_FAST_EXCEPTION;
            this->stop();
            return;
        case 0xF004:
            dispatch_integer_division_by_zero(*this, vcpu);
            return;
        default:
            this->callbacks.on_suspicious_activity("Illegal instruction");
            dispatch_illegal_instruction_violation(*this, vcpu);
            return;
        }
    }

    void windows_emulator::setup_x64_hooks(x86_64_emulator& emu)
    {
        emu.hook_instruction(x86_hookable_instructions::syscall, [&](cpu_interface& cpu, uint64_t) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            this->dispatcher.dispatch(*this, vcpu);
            return instruction_hook_continuation::skip_instruction;
        });

        // rdtsc/rdtscp are intercepted only when timing must stay exact (the analyzer). Otherwise the
        // guest reads the host TSC natively - the WHP backend serves the exit with the real TSC and no
        // kernel lock - removing a hot VM-exit path for games.
        if (this->intercept_rdtsc_ || !this->use_native_timestamp_counter_)
        {
            emu.hook_instruction(x86_hookable_instructions::rdtscp, [&](cpu_interface& cpu, uint64_t) {
                const std::scoped_lock lock(this->kernel_lock_);
                auto& vcpu = this->vcpu(cpu.index());
                const scoped_dispatch dispatch(*this, vcpu);
                auto& acting = vcpu.x64_cpu();
                this->callbacks.on_rdtscp();

                if (this->use_native_timestamp_counter_)
                {
                    return instruction_hook_continuation::run_instruction;
                }

                const auto ticks = this->clock_->timestamp_counter();
                acting.reg(x86_register::rax, static_cast<uint32_t>(ticks));
                acting.reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

                // Return the IA32_TSC_AUX value in RCX (low 32 bits)
                auto tsc_aux = 0; // Need to replace this with proper CPUID later
                acting.reg(x86_register::rcx, tsc_aux);

                return instruction_hook_continuation::skip_instruction;
            });

            emu.hook_instruction(x86_hookable_instructions::rdtsc, [&](cpu_interface& cpu, uint64_t) {
                const std::scoped_lock lock(this->kernel_lock_);
                auto& vcpu = this->vcpu(cpu.index());
                const scoped_dispatch dispatch(*this, vcpu);
                auto& acting = vcpu.x64_cpu();
                this->callbacks.on_rdtsc();

                if (this->use_native_timestamp_counter_)
                {
                    return instruction_hook_continuation::run_instruction;
                }

                const auto ticks = this->clock_->timestamp_counter();
                acting.reg(x86_register::rax, static_cast<uint32_t>(ticks));
                acting.reg(x86_register::rdx, static_cast<uint32_t>(ticks >> 32));

                return instruction_hook_continuation::skip_instruction;
            });
        }

        // TODO: Unicorn needs this - This should be handled in the backend
        emu.hook_instruction(x86_hookable_instructions::invalid, [&](cpu_interface& cpu, uint64_t) {
            const std::scoped_lock lock(this->kernel_lock_);
            // TODO: Unify icicle & unicorn handling
            dispatch_illegal_instruction_violation(*this, this->vcpu(cpu.index()));
            return instruction_hook_continuation::skip_instruction; //
        });

        emu.hook_interrupt([&](cpu_interface& cpu, const int interrupt) {
            const std::scoped_lock lock(this->kernel_lock_);
            auto& vcpu = this->vcpu(cpu.index());
            const scoped_dispatch dispatch(*this, vcpu);
            auto& acting = vcpu.x64_cpu();
            this->callbacks.on_exception();
            const auto eflags = acting.reg<uint32_t>(x86_register::eflags);

            switch (interrupt)
            {
            case 0:
                dispatch_integer_division_by_zero(*this, vcpu);
                return;
            case 1:
                if ((eflags & 0x100) != 0)
                {
                    acting.reg(x86_register::eflags, eflags & ~0x100);
                }

                this->callbacks.on_suspicious_activity("Singlestep");
                dispatch_single_step(*this, vcpu);
                return;
            case 3:
                this->callbacks.on_suspicious_activity("Breakpoint");
                dispatch_breakpoint(*this, vcpu);
                return;
            case 6:
                this->callbacks.on_suspicious_activity("Illegal instruction");
                dispatch_illegal_instruction_violation(*this, vcpu);
                return;
            case 41:
                this->callbacks.on_fast_fail(acting.reg<uint32_t>(x86_register::ecx));
                this->process.exit_status = STATUS_FAIL_FAST_EXCEPTION;
                this->stop();
                return;
            case 45:
                this->callbacks.on_suspicious_activity("DbgPrint");
                {
                    const auto cs_selector = acting.reg<uint16_t>(x86_register::cs);
                    const auto bitness = segment_utils::get_segment_bitness(acting, cs_selector);
                    const auto service = acting.reg<uint32_t>(x86_register::eax);

                    if (bitness && *bitness == segment_utils::segment_bitness::bit64 &&
                        (service == BREAKPOINT_PRINT || service == BREAKPOINT_LOAD_SYMBOLS || service == BREAKPOINT_UNLOAD_SYMBOLS ||
                         service == BREAKPOINT_COMMAND_STRING))
                    {
                        const auto ip = this->uses_instruction_precision() //
                                            ? vcpu.thread().current_ip
                                            : acting.read_instruction_pointer();
                        acting.reg(x86_register::rip, ip + 3);
                    }
                    else
                    {
                        dispatch_breakpoint(*this, vcpu);
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
    }

    void windows_emulator::on_basic_block_execution(vcpu_context& vcpu, const basic_block&)
    {
        auto& thread = vcpu.thread();

        // This path deliberately trades instruction precision for speed (one callback per block instead of
        // one per instruction), so we cannot account for individual instructions. Time-slice on a fixed number
        // of executed blocks instead.
        ++this->executed_instructions_;

        if (++thread.executed_blocks % MAX_BASIC_BLOCKS_PER_TIME_SLICE == 0)
        {
            this->yield_thread(vcpu);
        }
    }

    void windows_emulator::start(size_t count)
    {
        this->should_stop = false;
        this->last_stop_reason_ = stop_reason::none;
        this->last_stop_detail_.clear();
        this->setup_process_if_necessary();

        if (count > 0 && this->vcpu_count_ > 1)
        {
            throw std::invalid_argument("Instruction-count budgets require a single vCPU");
        }

        const auto use_count = count > 0;
        const auto start_instructions = this->executed_instructions_;
        const auto target_instructions = start_instructions + count;

        std::mutex interrupt_mutex{};
        std::condition_variable interrupt_cond{};
        std::thread interrupt_thread{};
        std::vector<std::thread> workers{};
        std::atomic<uint32_t> active_workers{0};

        const auto _ = utils::finally([&] {
            {
                std::unique_lock lock{interrupt_mutex};
                this->should_stop = true;
            }

            interrupt_cond.notify_all();

            for (uint32_t i = 0; i < this->vcpu_count_; ++i)
            {
                this->vcpu(i).cpu.stop();
            }

            for (auto& worker : workers)
            {
                if (worker.joinable())
                {
                    worker.join();
                }
            }

            if (interrupt_thread.joinable())
            {
                interrupt_thread.join();
            }
        });

        const bool preempt_vcpus = !this->uses_instruction_precision() && this->emu().get_guest_cpu(0).is_stop_thread_safe();
        const bool refresh_kusd = !this->intercept_kusd_;
        if (preempt_vcpus || refresh_kusd)
        {
            interrupt_thread = std::thread([&, preempt_vcpus, refresh_kusd] {
                while (!this->should_stop)
                {
                    std::unique_lock lock{interrupt_mutex};
                    interrupt_cond.wait_for(lock, std::chrono::milliseconds(20), [&] {
                        return this->should_stop.load(); //
                    });

                    if (this->should_stop)
                    {
                        break;
                    }

                    if (refresh_kusd)
                    {
                        const std::scoped_lock kernel_lock(this->kernel_lock_);
                        this->process.kusd.refresh();
                    }

                    if (preempt_vcpus)
                    {
                        // Under the kernel lock so this switch_thread/stop() pair can't straddle a vCPU's
                        // own scheduling step: perform_thread_switch consumes switch_thread (exchange to
                        // false) under the lock, and a preemption whose switch request lands before that
                        // consume while its stop() only lands inside the next quantum surfaces there as a
                        // stop with no pending switch - the exact shape of a fatal wind-down, tearing the
                        // whole run off at a random parked rip. Serialized against the scheduler, the pair
                        // lands either fully before the consume (plain early switch) or fully inside the
                        // running quantum (ordinary preemption), never split across it.
                        const std::scoped_lock kernel_lock(this->kernel_lock_);
                        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
                        {
                            auto& v = this->vcpu(i);
                            v.switch_thread = true;
                            v.cpu.stop();
                        }
                    }
                }
            });
        }

        if (this->vcpu_count_ > 1)
        {
            this->ui_backend_->pump_events();
            // One worker thread per vCPU; this thread pumps UI events until the run ends.
            active_workers = this->vcpu_count_;
            workers.reserve(this->vcpu_count_);

            for (uint32_t i = 0; i < this->vcpu_count_; ++i)
            {
                workers.emplace_back([this, i, &active_workers] {
                    try
                    {
                        this->vcpu_worker(this->vcpu(i));
                    }
                    catch (const std::exception& e)
                    {
                        this->log.error("vCPU %u worker terminated: %s\n", i, e.what());
                        this->stop();
                    }

                    --active_workers;
                });
            }

            while (active_workers.load() > 0)
            {
                this->ui_backend_->pump_events();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            this->dump_exception_trace();
            this->dump_lock_profile();
            return;
        }

        auto& vcpu = this->vcpu(0);

        std::unique_lock lock(this->kernel_lock_);

        while (!this->should_stop)
        {
            lock.unlock();
            this->ui_backend_->pump_events();
            lock.lock();

            if (vcpu.switch_thread || !vcpu.thread().is_thread_ready(*this))
            {
                if (!this->perform_thread_switch(vcpu, lock))
                {
                    break;
                }
            }

            // Guest code executes with the kernel lock released; hook callbacks
            // (syscalls, exceptions, exec hooks) re-acquire it on VM exit.
            lock.unlock();
            vcpu.cpu.start(count);
            lock.lock();

            if (!vcpu.switch_thread && !vcpu.cpu.has_violation())
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

        this->dump_exception_trace();
        this->dump_lock_profile();
    }

    void windows_emulator::deliver_raw_input(const process_context::raw_input_payload& payload, const hwnd explicit_target)
    {
        // Resolve the destination at delivery time: an explicit hwndTarget, else the foreground window
        // (raw input registered with a NULL target follows keyboard focus). If neither resolves to a live
        // window right now there is nowhere to deliver to, so skip without dropping the registration.
        const auto target = explicit_target != 0 ? explicit_target : this->process.foreground_window;
        auto* raw_win = this->process.windows.get(target);
        if (!raw_win)
        {
            return;
        }

        auto* thread = get_thread_by_id(this->process, raw_win->thread_id);
        if (!thread)
        {
            return;
        }

        const auto token = this->process.next_raw_input_token++;
        this->process.raw_inputs[token] = payload;

        // Bound the pending set: WM_INPUT is normally consumed at once by GetRawInputData, but if the guest
        // stops pumping, drop the oldest tokens so the map can't grow without limit.
        constexpr size_t max_pending_raw_inputs = 256;
        while (this->process.raw_inputs.size() > max_pending_raw_inputs)
        {
            this->process.raw_inputs.erase(this->process.raw_inputs.begin());
        }

        msg m{};
        m.window = target;
        m.message = WM_INPUT;
        m.wParam = RIM_INPUT;
        m.lParam = token;
        thread->post_message(*this, m);
    }

    void windows_emulator::deliver_raw_mouse_input(const int32_t dx, const int32_t dy, const uint16_t button_flags,
                                                   const uint16_t button_data)
    {
        this->deliver_raw_input({.keyboard = false, .dx = dx, .dy = dy, .mouse_buttons = button_flags, .mouse_button_data = button_data},
                                this->process.raw_mouse_target);
    }

    void windows_emulator::deliver_raw_keyboard_input(const uint16_t vkey, const uint16_t scan_code, const uint32_t message,
                                                      const bool extended)
    {
        this->deliver_raw_input({.keyboard = true, .vkey = vkey, .scan_code = scan_code, .key_message = message, .key_extended = extended},
                                this->process.raw_keyboard_target);
    }

    void windows_emulator::set_foreground_window(const hwnd window)
    {
        const auto previous = this->process.foreground_window;
        if (previous == window)
        {
            return;
        }

        this->process.foreground_window = window;

        if (const auto* previous_window = this->process.windows.get(previous))
        {
            if (auto* owner = get_thread_by_id(this->process, previous_window->thread_id); owner && owner->active_window == previous)
            {
                owner->set_active_window(0);
            }
        }

        if (const auto* new_window = this->process.windows.get(window))
        {
            if (auto* owner = get_thread_by_id(this->process, new_window->thread_id))
            {
                owner->set_active_window(window);
            }
        }
    }

    void windows_emulator::handle_ui_event(const ui_event& event)
    {
        const std::scoped_lock lock(this->kernel_lock_);
        this->handle_ui_event_locked(event);
    }

    void windows_emulator::handle_ui_event_locked(const ui_event& event)
    {
        this->kernel_lock_.assert_held();

        const auto hook_id = low_level_hook_id(event.message);
        auto& pending = this->process.pending_low_level_input;

        if (is_character_message(event.message) && this->process.drop_low_level_characters)
        {
            return;
        }

        if (hook_id == k_wh_keyboard_ll)
        {
            this->process.drop_low_level_characters = false;
        }

        std::vector<uint64_t> hooks{};
        if (hook_id != 0)
        {
            for (auto& thread : this->process.threads | std::views::values)
            {
                if (thread.is_terminated())
                {
                    continue;
                }

                for (const auto& hook : thread.low_level_hooks)
                {
                    if (hook.id == hook_id)
                    {
                        hooks.push_back(hook.handle);
                    }
                }
            }

            // The most recently installed hook runs first.
            std::ranges::sort(hooks, std::greater{});
        }

        // Input already waiting for hooks keeps everything after it waiting too, so the guest sees it in order.
        if (hooks.empty() && (pending.empty() || !is_input_message(event.message)))
        {
            this->deliver_ui_event(event);
            return;
        }

        auto [screen_x, screen_y] = std::pair{this->process.cursor_x, this->process.cursor_y};
        if (is_pointer_message(event.message) && !is_mouse_wheel_message(event.message))
        {
            std::tie(screen_x, screen_y) = this->cursor_after_pointer_event(event);
        }

        const auto now = this->clock().steady_now();
        pending.push_back(process_context::low_level_input{
            .id = this->process.next_low_level_input_id++,
            .event = event,
            .screen_x = screen_x,
            .screen_y = screen_y,
            .time = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()),
            .hooks = std::move(hooks),
            .waiting_since = now,
        });

        if (auto* owner = this->drain_low_level_input())
        {
            this->wake_thread(*owner);
        }
    }

    std::pair<emulator_thread*, const user_low_level_hook*> windows_emulator::find_low_level_hook(const uint64_t handle)
    {
        for (auto& thread : this->process.threads | std::views::values)
        {
            if (thread.is_terminated())
            {
                continue;
            }

            for (const auto& hook : thread.low_level_hooks)
            {
                if (hook.handle == handle)
                {
                    return {&thread, &hook};
                }
            }
        }

        return {};
    }

    // Windows passes input on to the next hook when a low-level hook does not return within LowLevelHooksTimeout, and
    // ignores the result of a hook that returns later. Since Windows 7 it also silently removes that hook, so a thread
    // that installs one and never pumps messages delays only the first input, not every one.
    emulator_thread* windows_emulator::drain_low_level_input()
    {
        constexpr auto low_level_hooks_timeout = std::chrono::seconds(1);

        auto& pending = this->process.pending_low_level_input;
        const auto now = this->clock().steady_now();
        while (!pending.empty())
        {
            auto& front = pending.front();
            const auto timed_out = now - front.waiting_since > low_level_hooks_timeout;
            if (front.in_progress && !timed_out)
            {
                return nullptr;
            }

            while (!front.in_progress && !front.hooks.empty() && !this->find_low_level_hook(front.hooks.front()).second)
            {
                front.hooks.erase(front.hooks.begin());
            }

            if (timed_out && !front.hooks.empty())
            {
                const auto stalled = front.hooks.front();
                for (auto& thread : this->process.threads | std::views::values)
                {
                    if (std::erase_if(thread.low_level_hooks,
                                      [stalled](const user_low_level_hook& hook) { return hook.handle == stalled; }) != 0)
                    {
                        std::erase(thread.running_low_level_inputs, front.id);
                    }
                }

                front.hooks.erase(front.hooks.begin());
                front.in_progress = false;
                front.waiting_since = now;
                continue;
            }

            if (front.hooks.empty())
            {
                const auto event = front.event;
                pending.erase(pending.begin());
                if (!pending.empty())
                {
                    pending.front().waiting_since = now;
                }

                this->deliver_ui_event(event);
                continue;
            }

            return this->find_low_level_hook(front.hooks.front()).first;
        }

        return nullptr;
    }

    std::optional<std::pair<process_context::low_level_input, user_low_level_hook>> windows_emulator::begin_low_level_hook(
        emulator_thread& thread)
    {
        if (!this->has_low_level_hook_work(thread.id))
        {
            return std::nullopt;
        }

        auto& front = this->process.pending_low_level_input.front();
        front.in_progress = true;
        front.waiting_since = this->clock().steady_now();
        thread.running_low_level_inputs.push_back(front.id);
        return std::pair{front, *this->find_low_level_hook(front.hooks.front()).second};
    }

    void windows_emulator::finish_low_level_hook(emulator_thread& thread, const bool swallow)
    {
        if (thread.running_low_level_inputs.empty())
        {
            return;
        }

        const auto id = thread.running_low_level_inputs.back();
        thread.running_low_level_inputs.pop_back();

        auto& pending = this->process.pending_low_level_input;
        if (pending.empty() || !pending.front().in_progress || pending.front().id != id)
        {
            return;
        }

        const auto now = this->clock().steady_now();
        auto& front = pending.front();
        front.in_progress = false;
        front.waiting_since = now;
        if (swallow)
        {
            this->process.drop_low_level_characters = is_key_down_message(front.event.message);
            pending.erase(pending.begin());

            while (this->process.drop_low_level_characters && !pending.empty() && is_character_message(pending.front().event.message))
            {
                pending.erase(pending.begin());
            }

            if (!pending.empty())
            {
                pending.front().waiting_since = now;
            }
        }
        else if (!front.hooks.empty())
        {
            front.hooks.erase(front.hooks.begin());
        }

        if (auto* owner = this->drain_low_level_input())
        {
            this->wake_thread(*owner);
        }
    }

    bool windows_emulator::has_low_level_hook_work(const uint32_t thread_id)
    {
        this->drain_low_level_input();

        const auto& pending = this->process.pending_low_level_input;
        if (pending.empty() || pending.front().in_progress || pending.front().hooks.empty())
        {
            return false;
        }

        const auto* owner = this->find_low_level_hook(pending.front().hooks.front()).first;
        return owner && owner->id == thread_id;
    }

    // Kick the vCPU running the thread so it promptly re-checks its message queue; if the thread is not running on
    // any vCPU right now, fall back to vCPU 0 to force a reschedule that can pick up the now-ready thread.
    void windows_emulator::wake_thread(emulator_thread& thread)
    {
        auto* running_on = find_vcpu_running_thread(*this, thread);
        auto& vcpu = running_on ? *running_on : this->vcpu(0);
        vcpu.switch_thread = true;
        vcpu.cpu.stop();
    }

    std::pair<int32_t, int32_t> windows_emulator::cursor_after_pointer_event(const ui_event& event) const
    {
        int32_t cursor_x = this->process.cursor_x;
        int32_t cursor_y = this->process.cursor_y;
        if (event.relative_motion || event.cursor_target)
        {
            if (event.cursor_target)
            {
                std::tie(cursor_x, cursor_y) = *event.cursor_target;
            }
            else
            {
                cursor_x += event.relative_motion->first;
                cursor_y += event.relative_motion->second;
            }
            if (const auto& clip = this->process.cursor_clip)
            {
                const auto left = static_cast<int32_t>(clip->left);
                const auto top = static_cast<int32_t>(clip->top);
                cursor_x = std::clamp(cursor_x, left, std::max(left, static_cast<int32_t>(clip->right) - 1));
                cursor_y = std::clamp(cursor_y, top, std::max(top, static_cast<int32_t>(clip->bottom) - 1));
            }
        }
        else if (const auto origin = get_client_origin_relative_to_ancestor(this->process, event.window, 0))
        {
            cursor_x = origin->x + point_x(event.lParam);
            cursor_y = origin->y + point_y(event.lParam);
        }

        return {cursor_x, cursor_y};
    }

    void windows_emulator::update_key_state(const ui_event& event)
    {
        const auto virtual_key = static_cast<uint8_t>(event.wParam & 0xFF);
        if (virtual_key == VK_PACKET)
        {
            return;
        }

        const bool down = event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN;
        const auto set = [&](const uint8_t key) {
            auto& state = this->process.key_state[key];
            if (down && (state & 0x80) == 0)
            {
                this->process.async_key_state[key] = 1;
                if (key == VK_CAPITAL || key == VK_NUMLOCK || key == VK_SCROLL)
                {
                    state ^= 0x01;
                }
            }
            state = static_cast<uint8_t>((down ? 0x80 : 0) | (state & 0x01));
        };

        const auto side = side_virtual_key(static_cast<uint32_t>(event.wParam & 0xFF), event.lParam);
        set(side);
        for (const auto& [generic, left, right] : modifier_sides)
        {
            if (side == left || side == right)
            {
                const bool any_down = ((this->process.key_state[left] | this->process.key_state[right]) & 0x80) != 0;
                if (any_down && (this->process.key_state[generic] & 0x80) == 0)
                {
                    this->process.async_key_state[generic] = 1;
                }
                this->process.key_state[generic] = any_down ? 0x80 : 0;
            }
        }
    }

    ui_event windows_emulator::resolve_injected_event(const ui_event& event) const
    {
        if (!event.injected)
        {
            return event;
        }

        auto resolved = event;
        const auto down = [&](const uint32_t key) { return (this->process.key_state[key] & 0x80) != 0; };

        if (is_keyboard_message(event.message))
        {
            const auto virtual_key = static_cast<uint32_t>(event.wParam & 0xFF);
            const bool up = event.message == WM_KEYUP || event.message == WM_SYSKEYUP;
            const bool alt = virtual_key == VK_MENU || down(VK_MENU);
            const bool system_key = (alt || virtual_key == VK_F10) && !down(VK_CONTROL);
            if (system_key)
            {
                resolved.message = up ? WM_SYSKEYUP : WM_SYSKEYDOWN;
            }
            else
            {
                resolved.message = up ? WM_KEYUP : WM_KEYDOWN;
            }

            constexpr uint64_t context_bit = 1ull << 29;
            constexpr uint64_t previous_state_bit = 1ull << 30;
            resolved.lParam &= ~(context_bit | previous_state_bit);
            resolved.lParam |= (alt && system_key) ? context_bit : 0;
            resolved.lParam |=
                (up || (virtual_key != VK_PACKET && down(side_virtual_key(virtual_key, event.lParam)))) ? previous_state_bit : 0;
            return resolved;
        }

        if (is_pointer_message(event.message))
        {
            uint64_t flags = 0;
            for (const auto& [key, flag] :
                 {std::pair{VK_LBUTTON, MK_LBUTTON}, std::pair{VK_RBUTTON, MK_RBUTTON}, std::pair{VK_SHIFT, MK_SHIFT},
                  std::pair{VK_CONTROL, MK_CONTROL}, std::pair{VK_MBUTTON, MK_MBUTTON}, std::pair{VK_XBUTTON1, MK_XBUTTON1},
                  std::pair{VK_XBUTTON2, MK_XBUTTON2}})
            {
                const bool pressed =
                    mouse_button_virtual_key(event.message, event.wParam) == key ? is_mouse_button_down_message(event.message) : down(key);
                flags |= pressed ? static_cast<uint64_t>(flag) : 0;
            }
            resolved.wParam = (event.wParam & 0xFFFF0000) | flags;
        }

        return resolved;
    }

    void windows_emulator::deliver_ui_event(const ui_event& queued_event)
    {
        const auto event = this->resolve_injected_event(queued_event);
        if (event.message == WM_SIZE)
        {
            this->resize_window_from_host(event.window, event.wParam == SIZE_MAXIMIZED, point_x(event.lParam), point_y(event.lParam));
            return;
        }

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

        const auto previous_foreground = this->process.foreground_window;

        msg m{};
        m.window = event.window;
        m.message = event.message;
        m.wParam = event.wParam;
        m.lParam = event.lParam;

        if (event.injected && is_keyboard_message(event.message) && thread->focus_window != 0)
        {
            if (const auto* focus = this->process.windows.get(thread->focus_window); focus && focus->thread_id == win->thread_id)
            {
                m.window = thread->focus_window;
            }
        }

        if (is_pointer_message(event.message))
        {
            // Track the cursor in screen coordinates (client origin + client-local position) so GetCursorPos
            // reflects it, and treat the window the user is pointing at as the foreground one.
            const auto origin = get_client_origin_relative_to_ancestor(this->process, event.window, 0);
            auto l_param = event.lParam;
            const auto [new_cursor_x, new_cursor_y] = this->cursor_after_pointer_event(event);
            int32_t raw_dx = new_cursor_x - this->process.cursor_x;
            int32_t raw_dy = new_cursor_y - this->process.cursor_y;
            if (event.relative_motion || event.cursor_target)
            {
                // Like a ClipCursor-confined cursor on Windows: the position stops at the clip rectangle while the
                // raw motion keeps flowing.
                if (event.relative_motion)
                {
                    raw_dx = event.relative_motion->first;
                    raw_dy = event.relative_motion->second;
                }
                if (origin)
                {
                    l_param = pack_point(new_cursor_x - origin->x, new_cursor_y - origin->y);
                }
            }

            // A game using raw input for mouse-look reads relative deltas from WM_INPUT, not WM_MOUSEMOVE.
            // Synthesize that delta from the change in tracked cursor position before updating it. A
            // SetCursorPos recenter pre-updates cursor_x/y, so the warp-echo motion event yields a zero
            // delta -- no spurious look -- while genuine motion between recenters produces the real delta.
            if (this->process.raw_mouse_registered)
            {
                if (event.message == WM_MOUSEMOVE)
                {
                    if (raw_dx != 0 || raw_dy != 0)
                    {
                        this->deliver_raw_mouse_input(raw_dx, raw_dy, 0);
                    }
                }
                else if (const uint16_t buttons = raw_mouse_button_flags(event.message, event.wParam); buttons != 0)
                {
                    // Raw-input games read button transitions and wheel deltas from WM_INPUT, not only
                    // the corresponding window messages. For wheel messages the delta lives in usButtonData.
                    this->deliver_raw_mouse_input(0, 0, buttons, raw_mouse_button_data(event.message, event.wParam));
                }
            }

            this->process.cursor_x = new_cursor_x;
            this->process.cursor_y = new_cursor_y;
            this->set_foreground_window(event.window);

            const auto target = route_pointer(this->process, event.window, point_x(l_param), point_y(l_param));
            m.window = target.window;
            // WM_MOUSEWHEEL/WM_MOUSEHWHEEL carry screen coordinates in lParam, unlike button/move
            // messages which carry client coordinates. Keep the routed target but preserve screen coords.
            m.lParam = is_mouse_wheel_message(event.message) ? pack_point(new_cursor_x, new_cursor_y) : pack_point(target.x, target.y);
        }
        else if ((event.message == WM_ACTIVATE && event.wParam != 0) || event.message == WM_SETFOCUS)
        {
            // The window just became active/focused: make it the foreground window so polling APIs
            // (GetForegroundWindow/GetActiveWindow) agree with the activation the game just received.
            this->set_foreground_window(event.window);
        }
        else if ((event.message == WM_ACTIVATE && event.wParam == 0) && this->process.foreground_window == event.window)
        {
            this->set_foreground_window(0);
        }

        // Mirror the foreground window into the shared SERVERINFO so the guest's client-side
        // GetForegroundWindow (which reads gpsi directly, never syscalling) returns the active window.
        // Fall back to the desktop window when no app window is active: real Windows always has a
        // foreground window, and code that needs a valid HWND (e.g. DirectSound's SetCooperativeLevel,
        // which Miles feeds from GetForegroundWindow) breaks on a null one.
        const auto foreground =
            this->process.foreground_window != 0 ? this->process.foreground_window : this->process.default_desktop_window_handle.bits;
        this->process.user_handles.get_server_info().access([&](USER_SERVERINFO& server_info) {
            server_info.foregroundWindow = foreground; //
        });

        // Maintain the polled key state from key and mouse-button transitions. GetKeyState reports the high
        // down bit; GetAsyncKeyState also reports a low edge bit that is set once when a key transitions from
        // up to down and cleared by the next GetAsyncKeyState query for that virtual key.
        switch (event.message)
        {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            this->update_key_state(event);
            break;
        default:
            if (const auto virtual_key = mouse_button_virtual_key(event.message, event.wParam); virtual_key != 0)
            {
                if (is_mouse_button_down_message(event.message))
                {
                    if ((this->process.key_state[virtual_key] & 0x80) == 0)
                    {
                        this->process.async_key_state[virtual_key] = 1;
                    }
                    this->process.key_state[virtual_key] = 0x80;
                }
                else if (is_mouse_button_up_message(event.message))
                {
                    this->process.key_state[virtual_key] = 0;
                }
            }
            break;
        }

        // Raw-input games (e.g. Skyrim) read the keyboard via WM_INPUT/GetRawInputData, not WM_KEYDOWN.
        if (this->process.raw_keyboard_registered && is_keyboard_message(event.message))
        {
            const auto vk = static_cast<uint16_t>(event.wParam & 0xFFFF);
            auto scan_code = static_cast<uint16_t>((event.lParam >> 16) & 0xFF);
            if (scan_code == 0)
            {
                scan_code = vk_to_scan_code(vk);
            }

            const bool extended = (event.lParam & (1ull << 24)) != 0;
            this->deliver_raw_keyboard_input(vk, scan_code, event.message, extended);
        }

        thread->post_message(*this, m, true);

        if (event.injected && is_key_down_message(event.message))
        {
            const auto virtual_key = static_cast<uint32_t>(event.wParam & 0xFF);
            const auto character =
                virtual_key == VK_PACKET ? event.injected_character : key_character(virtual_key, this->process.key_state);
            if (character != 0)
            {
                msg character_message = m;
                character_message.message = event.message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
                character_message.wParam = character;
                thread->post_message(*this, character_message, true);
            }
        }

        if (event.message == WM_CLOSE || event.message == WM_COMMAND || is_key_down_message(event.message) ||
            is_mouse_button_message(event.message) || is_mouse_wheel_message(event.message))
        {
            this->wake_thread(*thread);
        }

        if (this->process.foreground_window != previous_foreground)
        {
            this->apply_cursor_capture();
        }
    }

    void windows_emulator::resize_window_from_host(const hwnd window_handle, const bool maximized, const int32_t client_width,
                                                   const int32_t client_height)
    {
        auto* win = this->process.windows.get(window_handle);
        if (!win)
        {
            return;
        }

        const bool was_maximized = (win->style & WS_MAXIMIZE) != 0;
        if (win->client_width() == client_width && win->client_height() == client_height && was_maximized == maximized)
        {
            return;
        }

        auto* thread = get_thread_by_id(this->process, win->thread_id);
        if (!thread)
        {
            return;
        }

        const auto insets = win->nonclient_insets();
        win->width = client_width + insets.left + insets.right;
        win->height = client_height + insets.top + insets.bottom;
        win->style = maximized ? (win->style | WS_MAXIMIZE) : (win->style & ~WS_MAXIMIZE);
        win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win->style; });
        win->sync_guest_rects();

        win->update_rect = RECT{.left = 0, .top = 0, .right = win->client_width(), .bottom = win->client_height()};
        win->update_pending = true;
        win->erase_pending = true;
        thread->mark_queue_status_changed(QS_PAINT);

        msg m{};
        m.window = window_handle;
        m.message = WM_SIZE;
        m.wParam = maximized ? SIZE_MAXIMIZED : SIZE_RESTORED;
        m.lParam = pack_point(win->client_width(), win->client_height());
        thread->post_message(*this, m, true);
        this->wake_thread(*thread);
    }

    void windows_emulator::dump_exception_trace()
    {
        // Opt-in post-mortem aid for multi-vCPU debugging (see docs/multi-vcpu-design.md).
        const auto* enabled = std::getenv("SOGEN_TRACE_EXCEPTIONS");
        if (!enabled || enabled[0] != '1')
        {
            return;
        }

        const auto count = std::min(this->exception_trace_index_, this->exception_trace_.size());
        if (count == 0)
        {
            return;
        }

        const auto total = this->exception_trace_index_;
        const auto start = total - count;
        this->log.error("--- exception trace (last %zu of %zu) ---\n", count, total);

        const auto describe = [this](const uint64_t address) -> std::string {
            const auto* mod = this->mod_manager.find_by_address(address);
            const auto region = this->memory.get_region_info(address);
            std::array<char, 256> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s+0x%llx [%s]", mod ? mod->name.c_str() : "?",
                          mod ? static_cast<unsigned long long>(address - mod->image_base) : address,
                          region.is_committed ? "committed" : "FREE/reserved");
            return buffer.data();
        };

        for (size_t i = 0; i < count; ++i)
        {
            const auto& e = this->exception_trace_[(start + i) % this->exception_trace_.size()];
            this->log.error("  [%zu] status 0x%08x vcpu %u tid %u\n      rip  0x%llx %s\n      addr 0x%llx %s\n", start + i, e.status,
                            e.vcpu, e.tid, static_cast<unsigned long long>(e.rip), describe(e.rip).c_str(),
                            static_cast<unsigned long long>(e.info), describe(e.info).c_str());
        }
    }

    void windows_emulator::request_guest_event_signal(const handle event_handle, const uint32_t maximum_pending)
    {
        {
            const std::lock_guard lock{this->pending_event_signals_mutex_};
            auto& pending = this->pending_event_signals_[event_handle.bits];
            pending = std::min(pending + 1, maximum_pending);
        }

        this->credit_idle_wakeup();
    }

    void windows_emulator::apply_pending_guest_event_signals()
    {
        this->kernel_lock_.assert_held();

        const std::lock_guard lock{this->pending_event_signals_mutex_};

        for (auto it = this->pending_event_signals_.begin(); it != this->pending_event_signals_.end();)
        {
            auto* entry = this->process.events.get(make_handle(it->first));
            if (!entry)
            {
                it = this->pending_event_signals_.erase(it);
                continue;
            }

            if (!entry->signaled)
            {
                entry->signaled = true;
                if (--it->second == 0)
                {
                    it = this->pending_event_signals_.erase(it);
                    continue;
                }
            }

            ++it;
        }
    }

    void windows_emulator::dump_lock_profile()
    {
        if (!kernel_lock::profiling_enabled())
        {
            return;
        }

        const auto stats = this->kernel_lock_.profile();
        const auto held_ms = static_cast<double>(stats.held_nanos) / 1e6;
        const auto wait_ms = static_cast<double>(stats.wait_nanos) / 1e6;
        const auto contended_pct =
            stats.acquisitions ? (100.0 * static_cast<double>(stats.contended) / static_cast<double>(stats.acquisitions)) : 0.0;

        this->log.print(color::cyan,
                        "--- kernel lock (BEL) profile ---\n"
                        "  acquisitions:   %llu\n"
                        "  contended:      %llu (%.1f%%)\n"
                        "  wait time:      %.1f ms (blocked on a busy BEL)\n"
                        "  held time:      %.1f ms (BEL busy across all threads)\n",
                        static_cast<unsigned long long>(stats.acquisitions), static_cast<unsigned long long>(stats.contended),
                        contended_pct, wait_ms, held_ms);
    }

    void windows_emulator::stop()
    {
        {
            const std::lock_guard lock{this->idle_wakeup_mutex_};
            this->should_stop = true;
        }

        this->idle_wakeup_cv_.notify_all();
        this->thread_registers_saved_.notify_all();

        for (uint32_t i = 0; i < this->vcpu_count_; ++i)
        {
            this->vcpu(i).cpu.stop();
        }
    }

    void windows_emulator::wake_idle_vcpu()
    {
        if (this->idle_vcpus_.load() != 0)
        {
            this->credit_idle_wakeup();
        }
    }

    void windows_emulator::credit_idle_wakeup()
    {
        {
            const std::lock_guard lock{this->idle_wakeup_mutex_};
            this->pending_idle_wakeups_ = std::min(this->pending_idle_wakeups_ + 1, this->vcpu_count_);
        }

        this->idle_wakeup_cv_.notify_one();
    }

    bool windows_emulator::wait_for_idle_wakeup(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{this->idle_wakeup_mutex_};
        this->idle_wakeup_cv_.wait_for(lock, timeout, [this] { return this->pending_idle_wakeups_ > 0 || this->should_stop; });

        if (this->pending_idle_wakeups_ == 0)
        {
            return false;
        }

        --this->pending_idle_wakeups_;
        return true;
    }

    void windows_emulator::register_factories(utils::buffer_deserializer& buffer)
    {
        buffer.register_factory<memory_manager_wrapper>([this] {
            return memory_manager_wrapper{this->memory}; //
        });

        buffer.register_factory<module_manager_wrapper>([this] {
            return module_manager_wrapper{this->mod_manager}; //
        });

        buffer.register_factory<guest_machine_wrapper>([this] {
            return guest_machine_wrapper{this->emu()}; //
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

        buffer.register_factory<file_system_wrapper>([this] {
            return file_system_wrapper{this->file_sys}; //
        });

        buffer.register_factory<window>([this] {
            return window{this->emu()}; //
        });

        buffer.register_factory<menu>([this] { return menu{this->emu()}; });
        buffer.register_factory<accelerator_table>([this] { return accelerator_table{this->emu()}; });
    }

    void windows_emulator::request_suspend()
    {
        const std::scoped_lock lock(this->kernel_lock_);
        if (std::ranges::any_of(this->process.devices | std::views::values,
                                [](const io_device_container& device) { return device.suspends_at_frame_end(); }))
        {
            this->suspend_requested_ = true;
            return;
        }

        this->stop();
    }

    bool windows_emulator::take_suspend_request()
    {
        return this->suspend_requested_.exchange(false);
    }

    void windows_emulator::cancel_suspend_request()
    {
        this->suspend_requested_ = false;
    }

    void windows_emulator::ensure_serialization_support() const
    {
        for (const auto& device : this->process.devices | std::views::values)
        {
            if (!device.supports_serialization())
            {
                throw std::runtime_error("The device " + u16_to_u8(device.get_device_path()) + " does not support snapshots");
            }
        }
    }

    void windows_emulator::prepare_serialization()
    {
        this->ensure_serialization_support();
        for (auto& device : this->process.devices | std::views::values)
        {
            device.prepare_serialization(*this);
        }
    }

    void windows_emulator::serialize(utils::buffer_serializer& buffer) const
    {
        this->ensure_serialization_support();

        buffer.write(this->application_settings_);
        buffer.write(this->setup_completed_);
        buffer.write(this->executed_instructions_);
        buffer.write_atomic(this->vcpus_[0]->switch_thread);
        buffer.write(this->use_relative_time_);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Backend snapshot mode is not used here; Unicorn's in-place snapshot path is broken.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer, this->vcpus_[0]->active_thread);
        this->process.section_views.serialize(buffer, this->memory);
    }

    void windows_emulator::deserialize(utils::buffer_deserializer& buffer)
    {
        this->register_factories(buffer);

        buffer.read(this->application_settings_);
        buffer.read(this->setup_completed_);
        buffer.read(this->executed_instructions_);
        buffer.read_atomic(this->vcpus_[0]->switch_thread);

        const auto old_relative_time = this->use_relative_time_;
        buffer.read(this->use_relative_time_);

        if (old_relative_time != this->use_relative_time_)
        {
            throw std::runtime_error("Can not deserialize emulator with different time dimensions");
        }

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->process.prepare_for_state_restore(*this);
        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();
        this->ui().reset();
        this->audio().stop();
        this->console().reset();

        // Match raw serialize() above; do not use backend snapshot mode here.
        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        emulator_thread* restored_thread{};
        this->process.deserialize(buffer, restored_thread);
        this->vcpus_[0]->set_active_thread(restored_thread);
        this->process.section_views.deserialize(buffer);
        this->process.restore_after_state_restore(*this);
        this->ui().set_cursor_visibility(this->process.cursor_show_count >= 0 && this->process.cursor_shape_visible);
        this->apply_cursor_capture();
        this->release_held_input();
    }

    // Nothing is held when a saved process continues, so what was held when it was saved is released, the way the guest
    // would have seen it happen.
    void windows_emulator::release_held_input()
    {
        for (uint32_t key = 0; key < this->process.key_state.size(); ++key)
        {
            if ((this->process.key_state[key] & 0x80) == 0)
            {
                continue;
            }

            if (key == VK_LBUTTON || key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2)
            {
                this->process.key_state[key] = 0;
                continue;
            }

            const bool alt_held = (this->process.key_state[VK_MENU] & 0x80) != 0;
            constexpr uint64_t release_flags = 0xC0000001;
            this->handle_ui_event(ui_event{
                .window = this->process.foreground_window,
                .message = static_cast<uint32_t>(alt_held ? WM_SYSKEYUP : WM_KEYUP),
                .wParam = key,
                .lParam = release_flags | (static_cast<uint64_t>(vk_to_scan_code(static_cast<uint16_t>(key))) << 16),
            });

            // Delivering the release does not clear the key when there is no window to deliver it to.
            this->process.key_state[key] &= 0x01;
        }
    }

    uint64_t windows_emulator::highest_user_address() const
    {
        if (!this->process.is_wow64_process)
        {
            return MAX_ALLOCATION_END_EXCL - 1;
        }
        const auto* executable = this->mod_manager.executable;
        return (executable && (executable->characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)) ? 0xFFFEFFFF : 0x7FFEFFFF;
    }

    // The guest's ClipCursor is process-wide; the host confines whichever window is in the foreground, in relative mode while
    // the guest cursor is hidden.
    void windows_emulator::apply_cursor_capture()
    {
        std::optional<ui_rect> clip{};
        if (const auto& rect = this->process.cursor_clip)
        {
            clip = ui_rect{.left = rect->left, .top = rect->top, .right = rect->right, .bottom = rect->bottom};
        }

        // SDL's relative mode pins the cursor to a pixel in the window's center but may leave it visible. Only raw motion
        // reaches the guest then, just as with a hidden cursor.
        const bool hidden = this->process.cursor_show_count < 0 || !this->process.cursor_shape_visible;
        const bool pinned = clip && clip->right - clip->left <= 2 && clip->bottom - clip->top <= 2;
        this->ui().set_cursor_clip(this->process.foreground_window, clip, clip.has_value() && (hidden || pinned));
    }

    void windows_emulator::save_snapshot()
    {
        this->prepare_serialization();

        utils::buffer_serializer buffer{};

        buffer.write(this->setup_completed_);
        buffer.write(this->executed_instructions_);
        buffer.write_atomic(this->vcpus_[0]->switch_thread);

        this->version.serialize(buffer);
        this->registry.serialize_runtime_state(buffer);

        // Snapshot path still uses regular backend state serialization.
        // Backend snapshot mode (is_snapshot=true) is not reliable yet.
        this->emu().serialize_state(buffer, false);
        this->memory.serialize_memory_state(buffer, false);
        this->mod_manager.serialize(buffer);
        this->dispatcher.serialize(buffer);
        this->process.serialize(buffer, this->vcpus_[0]->active_thread);
        this->process.section_views.serialize(buffer, this->memory);

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
        buffer.read_atomic(this->vcpus_[0]->switch_thread);

        this->version.deserialize(buffer);
        this->registry.deserialize_runtime_state(buffer);

        this->process.prepare_for_state_restore(*this);
        this->memory.unmap_all_memory();
        this->clear_section_first_execution_hooks();
        this->ui().reset();
        this->audio().stop();
        this->console().reset();

        this->emu().deserialize_state(buffer, false);
        this->memory.deserialize_memory_state(buffer, false);
        this->mod_manager.deserialize(buffer);
        this->install_section_first_execution_hooks();
        this->dispatcher.deserialize(buffer);
        emulator_thread* restored_thread{};
        this->process.deserialize(buffer, restored_thread);
        this->vcpus_[0]->set_active_thread(restored_thread);
        this->process.section_views.deserialize(buffer);
        this->process.restore_after_state_restore(*this);
        this->ui().set_cursor_visibility(this->process.cursor_show_count >= 0 && this->process.cursor_shape_visible);
        this->apply_cursor_capture();
    }

} // namespace sogen
