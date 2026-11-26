#include "std_include.hpp"

namespace sogen::syscalls
{
    uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                             emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name, DWORD function_id,
                                             DWORD flags, emulator_pointer wow);
    hwnd handle_NtUserCreateWindowEx(const syscall_context& c, DWORD ex_style, emulator_object<LARGE_STRING> class_name,
                                     emulator_object<LARGE_STRING> cls_version, emulator_object<LARGE_STRING> window_name, DWORD style,
                                     int x, int y, int width, int height, hwnd parent, hmenu menu, hinstance instance, pointer l_param,
                                     DWORD flags, pointer acbi_buffer);
    BOOL handle_NtUserMoveWindow(const syscall_context& c, hwnd hwnd, int x, int y, int width, int height, BOOL repaint);
    uint64_t handle_NtUserMessageCall(const syscall_context& c, hwnd hwnd, UINT msg, uint64_t w_param, uint64_t l_param,
                                      uint64_t result_info, DWORD type, BOOL ansi);
    BOOL handle_NtUserSetWindowPos(const syscall_context& c, hwnd hWnd, hwnd hwnd_insert_after, int x, int y, int cx, int cy, UINT flags);
}

namespace sogen::test
{
    namespace
    {
        constexpr std::u16string_view class_name = u"SogenMinTrackTestClass";
        constexpr uint32_t unicode_class_flags = 0xC0;
        constexpr uint32_t overlapped_window = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        constexpr UINT move_flags = SWP_NOZORDER | SWP_NOACTIVATE;

        enum class window_proc
        {
            default_window_proc,
            keeps_own_size,
            lowers_min_track_size,
            raises_min_track_size,
            reports_default_minimum,
        };

        struct extent
        {
            int width;
            int height;
        };

        constexpr extent lowered_min_track_size{.width = 50, .height = 20};
        constexpr extent raised_min_track_size{.width = 400, .height = 300};

        struct max_payload_case
        {
            std::string_view name;
            uint32_t ex_style;
            int overhang;
        };

        constexpr std::array max_payload_cases{
            max_payload_case{"plain", 0, 8},
            max_payload_case{"client_edge", WS_EX_CLIENTEDGE, 10},
            max_payload_case{"static_edge", WS_EX_STATICEDGE, 9},
        };

        enum class parent_kind
        {
            none,
            child_of_popup,
            message_only,
        };

        struct create_case
        {
            std::string_view name;
            uint32_t style;
            uint32_t ex_style;
            parent_kind parent;
            int x;
            int y;
            int width;
            int height;
            int expected_width;
            int expected_height;
        };

        // Sizes GetWindowRect reports after CreateWindowEx on Windows 11 build 26300 (x64 and x86 identical).
        constexpr std::array create_cases{
            create_case{"overlapped_0x0", overlapped_window, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"overlapped_1x1", overlapped_window, 0, parent_kind::none, 0, 0, 1, 1, 136, 39},
            create_case{"overlapped_10x10", overlapped_window, 0, parent_kind::none, 0, 0, 10, 10, 136, 39},
            create_case{"overlapped_100x100", overlapped_window, 0, parent_kind::none, 0, 0, 100, 100, 136, 100},
            create_case{"overlapped_neg_w", overlapped_window, 0, parent_kind::none, 0, 0, -5, -5, 136, 39},
            create_case{"overlapped_at_50_60_0x0", overlapped_window, 0, parent_kind::none, 50, 60, 0, 0, 136, 39},
            create_case{"caption_only_0x0", WS_CAPTION, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"caption_sysmenu_0x0", WS_CAPTION | WS_SYSMENU, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"thickframe_only_0x0", WS_THICKFRAME, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"border_only_0x0", WS_BORDER, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"ov_dlgframe_0x0", WS_DLGFRAME, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"ov_clientedge_0x0", WS_OVERLAPPED, WS_EX_CLIENTEDGE, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"ov_plain_0x0", WS_OVERLAPPED, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"tool_cap_100x40", WS_CAPTION, WS_EX_TOOLWINDOW, parent_kind::none, 0, 0, 100, 40, 100, 40},
            create_case{"tool_popup_thick_border_100x40", WS_POPUP | WS_THICKFRAME | WS_BORDER, WS_EX_TOOLWINDOW, parent_kind::none, 0, 0,
                        100, 40, 100, 40},
            create_case{"ov_minbox_0x0", WS_OVERLAPPED | WS_MINIMIZEBOX, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"popup_0x0", WS_POPUP, 0, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_cap_0x0", WS_POPUP | WS_CAPTION, 0, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_dlgframe_0x0", WS_POPUP | WS_DLGFRAME, 0, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_border_0x0", WS_POPUP | WS_BORDER, 0, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_clientedge_0x0", WS_POPUP, WS_EX_CLIENTEDGE, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_dlgmodalframe_0x0", WS_POPUP, WS_EX_DLGMODALFRAME, parent_kind::none, 0, 0, 0, 0, 0, 0},
            create_case{"popup_thick_border_0x0", WS_POPUP | WS_THICKFRAME | WS_BORDER, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"popup_thick_dlgframe_0x0", WS_POPUP | WS_THICKFRAME | WS_DLGFRAME, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"popup_cap_thick_0x0", WS_POPUP | WS_CAPTION | WS_THICKFRAME, 0, parent_kind::none, 0, 0, 0, 0, 136, 39},
            create_case{"popup_cap_thick_sys_0x0", WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU, 0, parent_kind::none, 0, 0, 0, 0,
                        136, 39},
            create_case{"child_0x0", WS_CHILD, 0, parent_kind::child_of_popup, 0, 0, 0, 0, 0, 0},
            create_case{"child_cap_0x0", WS_CHILD | WS_CAPTION, 0, parent_kind::child_of_popup, 0, 0, 0, 0, 0, 0},
            create_case{"child_border_0x0", WS_CHILD | WS_BORDER, 0, parent_kind::child_of_popup, 0, 0, 0, 0, 0, 0},
            create_case{"child_thick_border_0x0", WS_CHILD | WS_THICKFRAME | WS_BORDER, 0, parent_kind::child_of_popup, 0, 0, 0, 0, 136,
                        39},
            create_case{"child_cap_thick_0x0", WS_CHILD | WS_CAPTION | WS_THICKFRAME, 0, parent_kind::child_of_popup, 0, 0, 0, 0, 136, 39},
            create_case{"child_cap_thick_sys_0x0", WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU, 0, parent_kind::child_of_popup, 0, 0,
                        0, 0, 136, 39},
            create_case{"msgonly_ov_0x0", overlapped_window, 0, parent_kind::message_only, 0, 0, 0, 0, 136, 39},
            create_case{"msgonly_0_0x0", 0, 0, parent_kind::message_only, 0, 0, 0, 0, 136, 39},
            create_case{"msgonly_thick_0x0", WS_THICKFRAME, 0, parent_kind::message_only, 0, 0, 0, 0, 136, 39},
            create_case{"msgonly_popup_0x0", WS_POPUP, 0, parent_kind::message_only, 0, 0, 0, 0, 0, 0},
            create_case{"child_cap_thick_5x5", WS_CHILD | WS_CAPTION | WS_THICKFRAME, 0, parent_kind::child_of_popup, 0, 0, 5, 5, 136, 39},
        };

        struct change_case
        {
            std::string_view name;
            uint32_t style;
            uint32_t ex_style;
            parent_kind parent;
            int width_to_0x0;
            int height_to_0x0;
            int width_to_5x5;
            int height_to_5x5;
        };

        // Sizes on Windows 11 build 26300 after MoveWindow(20, 30, 0, 0), SetWindowPos(40, 50, 0, 0) and SetWindowPos(10, 10, 5, 5).
        constexpr std::array change_cases{
            change_case{"ov_0x0", overlapped_window, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"ov_dlgframe_0x0", WS_DLGFRAME, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"ov_clientedge_0x0", WS_OVERLAPPED, WS_EX_CLIENTEDGE, parent_kind::none, 136, 39, 136, 39},
            change_case{"ov_plain_0x0", WS_OVERLAPPED, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"popup_cap_0x0", WS_POPUP | WS_CAPTION, 0, parent_kind::none, 0, 0, 5, 5},
            change_case{"popup_dlgframe_0x0", WS_POPUP | WS_DLGFRAME, 0, parent_kind::none, 0, 0, 5, 5},
            change_case{"popup_border_0x0", WS_POPUP | WS_BORDER, 0, parent_kind::none, 0, 0, 5, 5},
            change_case{"popup_thick_border_0x0", WS_POPUP | WS_THICKFRAME | WS_BORDER, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"popup_thick_dlgframe_0x0", WS_POPUP | WS_THICKFRAME | WS_DLGFRAME, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"popup_cap_thick_0x0", WS_POPUP | WS_CAPTION | WS_THICKFRAME, 0, parent_kind::none, 136, 39, 136, 39},
            change_case{"popup_cap_thick_sys_0x0", WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU, 0, parent_kind::none, 136, 39, 136,
                        39},
            change_case{"child_0x0", WS_CHILD, 0, parent_kind::child_of_popup, 0, 0, 5, 5},
            change_case{"child_cap_0x0", WS_CHILD | WS_CAPTION, 0, parent_kind::child_of_popup, 0, 0, 5, 5},
            change_case{"child_border_0x0", WS_CHILD | WS_BORDER, 0, parent_kind::child_of_popup, 0, 0, 5, 5},
            change_case{"child_thick_border_0x0", WS_CHILD | WS_THICKFRAME | WS_BORDER, 0, parent_kind::child_of_popup, 136, 39, 136, 39},
            change_case{"child_cap_thick_0x0", WS_CHILD | WS_CAPTION | WS_THICKFRAME, 0, parent_kind::child_of_popup, 136, 39, 136, 39},
            change_case{"msgonly_ov_0x0", overlapped_window, 0, parent_kind::message_only, 136, 39, 136, 39},
            change_case{"msgonly_0_0x0", 0, 0, parent_kind::message_only, 136, 39, 136, 39},
            change_case{"msgonly_thick_0x0", WS_THICKFRAME, 0, parent_kind::message_only, 136, 39, 136, 39},
            change_case{"msgonly_popup_0x0", WS_POPUP, 0, parent_kind::message_only, 0, 0, 5, 5},
        };

        class WindowMinTrackSizeTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                this->storage = this->emu.memory.allocate_memory(0x1000, memory_permission::read_write);
                this->atom = this->register_class();
                ASSERT_NE(this->atom, 0);
                this->parent = this->create_window(WS_POPUP, 0, 0, 0, 800, 600, 0);
                ASSERT_NE(this->parent, nullptr);
            }

            uint16_t register_class()
            {
                const emulator_object<EMU_WNDCLASSEX> wnd_class{this->emu.memory, this->storage};
                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> name{this->emu.memory, this->storage + 0x100};
                const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name{this->emu.memory, this->storage + 0x200};
                const auto name_buffer = this->storage + 0x300;

                wnd_class.write({.cbSize = sizeof(EMU_WNDCLASSEX),
                                 .style = 0,
                                 .lpfnWndProc = 0x140001000,
                                 .cbClsExtra = 0,
                                 .cbWndExtra = 0,
                                 .hInstance = 0x140000000,
                                 .hIcon = 0,
                                 .hCursor = 0,
                                 .hbrBackground = 0,
                                 .lpszMenuName = 0,
                                 .lpszClassName = 0,
                                 .hIconSm = 0});
                this->emu.memory.write_memory(name_buffer, class_name.data(), class_name.size() * sizeof(char16_t));
                name.write({.Length = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .MaximumLength = static_cast<USHORT>(class_name.size() * sizeof(char16_t)),
                            .Buffer = name_buffer});
                class_menu_name.write({});

                return syscalls::handle_NtUserRegisterClassExWOW(this->context(), wnd_class, name, {this->emu.memory}, class_menu_name, 0,
                                                                 unicode_class_flags, 0);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            // The creation messages run as guest callbacks. The window holds its geometry for the default minimum before the
            // first one, WM_GETMINMAXINFO, whose reply may change it.
            window* create_window(const uint32_t style, const uint32_t ex_style, const int x, const int y, const int width,
                                  const int height, const hwnd parent_handle, const window_proc proc = window_proc::default_window_proc)
            {
                std::set<std::remove_cvref_t<decltype(this->emu.process.windows.begin()->first)>> existing{};
                for (const auto& [index, win] : this->emu.process.windows)
                {
                    existing.insert(index);
                }

                (void)syscalls::handle_NtUserCreateWindowEx(this->context(), ex_style, {this->emu.memory, this->atom}, {this->emu.memory},
                                                            {this->emu.memory}, style, x, y, width, height, parent_handle, 0, 0x140000000,
                                                            0, 0, 0);

                for (auto& [index, win] : this->emu.process.windows)
                {
                    if (!existing.contains(index))
                    {
                        if (proc != window_proc::default_window_proc)
                        {
                            this->reply_to_creation_min_max_info(proc);
                        }
                        return &win;
                    }
                }

                return nullptr;
            }

            hwnd parent_handle(const parent_kind kind) const
            {
                switch (kind)
                {
                case parent_kind::child_of_popup:
                    return this->parent->handle;
                case parent_kind::message_only:
                    return EMU_HWND_MESSAGE;
                default:
                    return 0;
                }
            }

            window* create_window(const create_case& test)
            {
                return this->create_window(test.style, test.ex_style, test.x, test.y, test.width, test.height,
                                           this->parent_handle(test.parent));
            }

            callback_frame finish_callback()
            {
                auto& callbacks = this->emu.vcpu(0).active_thread->callback_stack;
                auto frame = std::move(callbacks.at(callbacks.size() - 1));
                callbacks.pop_back();
                frame.restore_registers(this->emu.vcpu(0).cpu);
                return frame;
            }

            static void set_syscall_arguments(const syscall_context& c, const std::initializer_list<uint64_t> arguments)
            {
                auto& cpu = c.x64_cpu();
                constexpr std::array register_arguments{x86_register::r10, x86_register::rdx, x86_register::r8, x86_register::r9};
                size_t index{};
                for (const auto argument : arguments)
                {
                    if (index < register_arguments.size())
                    {
                        cpu.reg(register_arguments[index], argument);
                    }
                    else
                    {
                        cpu.write_stack(index + 1, argument);
                    }

                    ++index;
                }
            }

            bool complete_callback(callback_frame& frame, const user_callback_result& result)
            {
                const auto status = this->emu.dispatcher.dispatch_completion(this->emu, this->emu.vcpu(0), frame.handler_id,
                                                                             frame.state.get(), result, frame.trap_stack_pointer);
                EXPECT_NE(dispatch_result::error, status);
                if (status == dispatch_result::completed)
                {
                    emulator_stack_leak_collector leak_collector{};
                    frame.state.reset();
                    EXPECT_NO_THROW(leak_collector.throw_if_leaked());
                }

                return status == dispatch_result::new_callback;
            }

            user_callback_result window_proc_answer_min_max_info(const uint64_t min_max_info_address, const window_proc proc)
            {
                EMU_MINMAXINFO info{};
                this->emu.memory.read_memory(min_max_info_address, &info, sizeof(info));
                this->received_min_max_info = info;
                EXPECT_EQ(136, info.ptMinTrackSize.x);
                EXPECT_EQ(39, info.ptMinTrackSize.y);

                if (proc == window_proc::lowers_min_track_size)
                {
                    info.ptMinTrackSize = {.x = lowered_min_track_size.width, .y = lowered_min_track_size.height};
                }
                else if (proc == window_proc::raises_min_track_size)
                {
                    info.ptMinTrackSize = {.x = raised_min_track_size.width, .y = raised_min_track_size.height};
                }

                this->emu.memory.write_memory(min_max_info_address, &info, sizeof(info));
                return {.value = 0, .output_size = sizeof(info), .output = min_max_info_address};
            }

            void reply_to_creation_min_max_info(const window_proc proc)
            {
                auto frame = this->finish_callback();
                const auto& state = dynamic_cast<const window_create_state&>(*frame.state);
                ASSERT_EQ(window_create_phase::min_max_info, state.phase);

                (void)this->complete_callback(frame, this->window_proc_answer_min_max_info(state.min_max_info_alloc.address(), proc));
            }

            void run_window_proc(const window& win, const uint64_t window_pos_address, const window_proc proc)
            {
                if (proc == window_proc::keeps_own_size)
                {
                    EMU_WINDOWPOS position{};
                    this->emu.memory.read_memory(window_pos_address, &position, sizeof(position));
                    position.cx = 50;
                    position.cy = 20;
                    this->emu.memory.write_memory(window_pos_address, &position, sizeof(position));
                    return;
                }

                constexpr uint64_t result_sentinel = 0xDEADBEEF;
                constexpr uint64_t return_target_sentinel = 0xC0DEC0DE;
                constexpr size_t caller_frame_slots = 8;
                const auto result_address = this->storage + 0x800;
                const auto return_target = this->storage + 0x900;
                this->emu.memory.write_memory(result_address, &result_sentinel, sizeof(result_sentinel));
                this->emu.memory.write_memory(return_target, &return_target_sentinel, sizeof(return_target_sentinel));

                auto c = this->context();
                auto& cpu = c.x64_cpu();
                const auto position_change_stack_pointer = cpu.read_stack_pointer();
                const auto stack_pointer = ((position_change_stack_pointer - 0x100) & ~uint64_t{0xF}) - 8;
                cpu.reg(x86_register::rsp, stack_pointer);
                cpu.write_stack(0, return_target);
                set_syscall_arguments(c, {win.handle, WM_WINDOWPOSCHANGING, 0, window_pos_address, result_address, FNID_DEFWINDOW, 0});

                std::array<uint64_t, caller_frame_slots> caller_frame{};
                this->emu.memory.read_memory(stack_pointer, caller_frame.data(), sizeof(caller_frame));

                (void)syscalls::handle_NtUserMessageCall(c, win.handle, WM_WINDOWPOSCHANGING, 0, window_pos_address, result_address,
                                                         FNID_DEFWINDOW, FALSE);
                if (!c.run_callback)
                {
                    cpu.reg(x86_register::rsp, position_change_stack_pointer);
                    return;
                }

                auto frame = this->finish_callback();
                const auto& state = dynamic_cast<const message_call_state&>(*frame.state);
                EXPECT_FALSE(
                    this->complete_callback(frame, this->window_proc_answer_min_max_info(state.min_max_info_alloc.address(), proc)));

                uint64_t result{result_sentinel};
                this->emu.memory.read_memory(result_address, &result, sizeof(result));
                EXPECT_EQ(0, result);

                uint64_t return_target_content{};
                this->emu.memory.read_memory(return_target, &return_target_content, sizeof(return_target_content));
                EXPECT_EQ(return_target_sentinel, return_target_content);

                std::array<uint64_t, caller_frame_slots> caller_frame_after{};
                this->emu.memory.read_memory(stack_pointer, caller_frame_after.data(), sizeof(caller_frame_after));
                EXPECT_EQ(caller_frame, caller_frame_after);
                EXPECT_EQ(stack_pointer, cpu.read_stack_pointer());
                cpu.reg(x86_register::rsp, position_change_stack_pointer);
            }

            template <typename StartChange>
            void run_position_change(const window& win, const window_proc proc, const StartChange& start_change)
            {
                bool callback_pending{};
                {
                    auto c = this->context();
                    (void)start_change(c);
                    callback_pending = c.run_callback;
                }

                while (callback_pending)
                {
                    auto frame = this->finish_callback();
                    const auto& state = dynamic_cast<const window_position_state&>(*frame.state);
                    if (!state.position_applied)
                    {
                        this->run_window_proc(win, state.window_pos_alloc.address(), proc);
                    }

                    callback_pending = this->complete_callback(frame, {});
                }
            }

            void set_window_pos(const window& win, const int x, const int y, const int width, const int height, const UINT flags,
                                const window_proc proc = window_proc::default_window_proc)
            {
                this->run_position_change(win, proc, [&](const syscall_context& c) {
                    set_syscall_arguments(c, {win.handle, 0, static_cast<uint64_t>(x), static_cast<uint64_t>(y),
                                              static_cast<uint64_t>(width), static_cast<uint64_t>(height), flags});
                    return syscalls::handle_NtUserSetWindowPos(c, win.handle, 0, x, y, width, height, flags);
                });
            }

            void move_window(const window& win, const int x, const int y, const int width, const int height,
                             const window_proc proc = window_proc::default_window_proc)
            {
                this->run_position_change(win, proc, [&](const syscall_context& c) {
                    set_syscall_arguments(c, {win.handle, static_cast<uint64_t>(x), static_cast<uint64_t>(y), static_cast<uint64_t>(width),
                                              static_cast<uint64_t>(height), FALSE});
                    return syscalls::handle_NtUserMoveWindow(c, win.handle, x, y, width, height, FALSE);
                });
            }

            void expect_received_max_payload(const int overhang)
            {
                const auto& metrics = this->emu.process.user_handles.get_server_info().read().systemMetrics;
                EXPECT_EQ(-overhang, this->received_min_max_info.ptMaxPosition.x);
                EXPECT_EQ(-overhang, this->received_min_max_info.ptMaxPosition.y);
                EXPECT_EQ(metrics[0] + (2 * overhang), this->received_min_max_info.ptMaxSize.x);
                EXPECT_EQ(metrics[1] + (2 * overhang), this->received_min_max_info.ptMaxSize.y);
            }

            void resize_to_300x200(const window& win)
            {
                this->move_window(win, 20, 30, 300, 200);
                ASSERT_EQ(300, win.width);
                ASSERT_EQ(200, win.height);
            }

            windows_emulator emu = create_sample_emulator();
            EMU_MINMAXINFO received_min_max_info{};
            uint64_t storage{};
            uint16_t atom{};
            window* parent{};
        };
    }

    TEST_F(WindowMinTrackSizeTest, CreationClampsTheRequestedSize)
    {
        for (const auto& test : create_cases)
        {
            SCOPED_TRACE(test.name);
            const auto* win = this->create_window(test);
            ASSERT_NE(win, nullptr);
            EXPECT_EQ(test.x, win->x);
            EXPECT_EQ(test.y, win->y);
            EXPECT_EQ(test.expected_width, win->width);
            EXPECT_EQ(test.expected_height, win->height);
        }
    }

    TEST_F(WindowMinTrackSizeTest, SizeChangesClampInTheDefaultWindowProcedure)
    {
        for (const auto& test : change_cases)
        {
            SCOPED_TRACE(test.name);
            const auto* win = this->create_window(test.style, test.ex_style, 0, 0, 300, 200, this->parent_handle(test.parent));
            ASSERT_NE(win, nullptr);
            ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));

            this->move_window(*win, 20, 30, 0, 0);
            EXPECT_EQ(20, win->x);
            EXPECT_EQ(30, win->y);
            EXPECT_EQ(test.width_to_0x0, win->width);
            EXPECT_EQ(test.height_to_0x0, win->height);

            ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));
            this->set_window_pos(*win, 40, 50, 0, 0, move_flags);
            EXPECT_EQ(40, win->x);
            EXPECT_EQ(50, win->y);
            EXPECT_EQ(test.width_to_0x0, win->width);
            EXPECT_EQ(test.height_to_0x0, win->height);

            ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));
            this->set_window_pos(*win, 40, 50, 0, 0, move_flags | SWP_NOSENDCHANGING);
            EXPECT_EQ(40, win->x);
            EXPECT_EQ(50, win->y);
            EXPECT_EQ(0, win->width);
            EXPECT_EQ(0, win->height);

            ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));
            this->set_window_pos(*win, 10, 10, 5, 5, move_flags);
            EXPECT_EQ(10, win->x);
            EXPECT_EQ(10, win->y);
            EXPECT_EQ(test.width_to_5x5, win->width);
            EXPECT_EQ(test.height_to_5x5, win->height);
        }

        auto* popup = this->create_window(WS_POPUP, 0, 0, 0, 0, 0, 0);
        ASSERT_NE(popup, nullptr);
        popup->style = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU;

        this->set_window_pos(*popup, 0, 0, 0, 0, move_flags | SWP_NOSIZE | SWP_NOMOVE | SWP_FRAMECHANGED);
        EXPECT_EQ(0, popup->width);
        EXPECT_EQ(0, popup->height);

        this->set_window_pos(*popup, 7, 8, 0, 0, move_flags | SWP_NOSIZE);
        EXPECT_EQ(7, popup->x);
        EXPECT_EQ(8, popup->y);
        EXPECT_EQ(0, popup->width);
        EXPECT_EQ(0, popup->height);
    }

    TEST_F(WindowMinTrackSizeTest, WindowProceduresDecideTheMinimum)
    {
        const auto* sized = this->create_window(overlapped_window, 0, 0, 0, 300, 200, 0);
        ASSERT_NE(sized, nullptr);

        this->set_window_pos(*sized, 10, 10, 300, 200, move_flags, window_proc::keeps_own_size);
        EXPECT_EQ(50, sized->width);
        EXPECT_EQ(20, sized->height);

        this->set_window_pos(*sized, 10, 10, 50, 20, move_flags);
        EXPECT_EQ(136, sized->width);
        EXPECT_EQ(39, sized->height);

        this->move_window(*sized, 20, 30, 300, 200, window_proc::keeps_own_size);
        EXPECT_EQ(50, sized->width);
        EXPECT_EQ(20, sized->height);

        this->move_window(*sized, 20, 30, 50, 20);
        EXPECT_EQ(136, sized->width);
        EXPECT_EQ(39, sized->height);

        const auto* caption = this->create_window(WS_CAPTION, 0, 0, 0, 100, 100, 0, window_proc::lowers_min_track_size);
        ASSERT_NE(caption, nullptr);
        EXPECT_EQ(100, caption->width);
        EXPECT_EQ(100, caption->height);

        const auto* raised = this->create_window(WS_CAPTION, 0, 0, 0, 10, 10, 0, window_proc::raises_min_track_size);
        ASSERT_NE(raised, nullptr);
        EXPECT_EQ(raised_min_track_size.width, raised->width);
        EXPECT_EQ(raised_min_track_size.height, raised->height);

        this->set_window_pos(*sized, 10, 10, 100, 100, move_flags, window_proc::lowers_min_track_size);
        EXPECT_EQ(100, sized->width);
        EXPECT_EQ(100, sized->height);

        this->move_window(*sized, 20, 30, 100, 100, window_proc::lowers_min_track_size);
        EXPECT_EQ(100, sized->width);
        EXPECT_EQ(100, sized->height);

        this->set_window_pos(*sized, 10, 10, 200, 150, move_flags, window_proc::raises_min_track_size);
        EXPECT_EQ(raised_min_track_size.width, sized->width);
        EXPECT_EQ(raised_min_track_size.height, sized->height);

        for (const auto& test : max_payload_cases)
        {
            SCOPED_TRACE(test.name);
            auto* win = this->create_window(overlapped_window, test.ex_style, 0, 0, 300, 200, 0, window_proc::reports_default_minimum);
            ASSERT_NE(win, nullptr);
            this->expect_received_max_payload(test.overhang);

            this->received_min_max_info = {};
            this->move_window(*win, 20, 30, 300, 200);
            this->expect_received_max_payload(test.overhang);
        }
    }

    TEST_F(WindowMinTrackSizeTest, ToolWindowsKeepASmallSize)
    {
        const auto* win = this->create_window(WS_CAPTION | WS_SYSMENU, WS_EX_TOOLWINDOW, 0, 0, 100, 40, 0);
        ASSERT_NE(win, nullptr);
        EXPECT_EQ(100, win->width);
        EXPECT_EQ(40, win->height);

        ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));
        this->move_window(*win, 20, 30, 100, 40);
        EXPECT_EQ(100, win->width);
        EXPECT_EQ(40, win->height);

        ASSERT_NO_FATAL_FAILURE(this->resize_to_300x200(*win));
        this->set_window_pos(*win, 40, 50, 100, 40, move_flags);
        EXPECT_EQ(100, win->width);
        EXPECT_EQ(40, win->height);
    }
}
