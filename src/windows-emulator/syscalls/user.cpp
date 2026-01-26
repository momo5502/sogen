#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "windows-emulator/user_callback_dispatch.hpp"

namespace
{
    void set_guest_last_error(const syscall_context& c, uint32_t last_error)
    {
        c.proc.active_thread->teb64->access([&](TEB64& teb) {
            teb.LastErrorValue = static_cast<ULONG>(last_error); //
        });
    }

    template <typename T>
    void dispatch_window_message(const syscall_context& c, callback_id id, T&& state, const window& win, uint32_t message,
                                 uint64_t w_param = 0, uint64_t l_param = 0)
    {
        c.proc.active_thread->teb64->access([&](TEB64& teb) {
            teb.Win32ClientInfo.arr[8] = win.handle;
            teb.Win32ClientInfo.arr[9] = win.guest.value();
        });

        dispatch_user_callback(c, id, std::forward<T>(state), c.proc.dispatch_client_message, win.guest.value(),
                               static_cast<uint64_t>(message), w_param, l_param, win.wnd_proc);
    }

    template <typename T>
    void dispatch_next_message(const syscall_context& c, callback_id id, T&& state, const window& win, std::vector<qmsg>& message_queue)
    {
        const auto m = message_queue.back();
        message_queue.pop_back();

        dispatch_window_message(c, id, std::forward<T>(state), win, m.message, m.wParam, m.lParam);
    }
}

namespace syscalls
{
    NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserRegisterWindowMessage()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetThreadState()
    {
        return 0;
    }

    hdc handle_NtUserGetDCEx(const syscall_context& /*c*/, const hwnd window, const uint64_t /*clip_region*/, const ULONG /*flags*/)
    {
        return window;
    }

    hdc handle_NtUserGetDC(const syscall_context& c, const hwnd window)
    {
        return handle_NtUserGetDCEx(c, window, 0, 0);
    }

    NTSTATUS handle_NtUserGetWindowDC()
    {
        return 1;
    }

    NTSTATUS handle_NtUserReleaseDC()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserGetCursorPos()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserSetCursor()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserFindExistingCursorIcon()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserFindWindowEx(const syscall_context& c, const hwnd, const hwnd,
                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> window_name)
    {
        if (c.win_emu.callbacks.on_generic_activity)
        {
            std::string class_name_str = "(null)";
            std::string window_name_str = "(null)";

            if (class_name)
            {
                class_name_str = u16_to_u8(read_unicode_string(c.emu, class_name));
            }

            if (window_name)
            {
                window_name_str = u16_to_u8(read_unicode_string(c.emu, window_name));
            }

            c.win_emu.callbacks.on_generic_activity("Window query for class '" + class_name_str + "' and name '" + window_name_str + "'");
        }

        return 0;
    }

    NTSTATUS handle_NtUserMoveWindow()
    {
        return 0;
    }

    NTSTATUS handle_NtUserGetProcessWindowStation()
    {
        return 0;
    }

    NTSTATUS handle_NtUserRegisterClassExWOW(const syscall_context& c, const emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class_version*/,
                                             const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name,
                                             const DWORD /*function_id*/, const DWORD /*flags*/, const emulator_pointer /*wow*/)
    {
        if (!wnd_class_ex)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const auto class_name_str = read_unicode_string(c.emu, class_name);
        const auto index = c.proc.add_or_find_atom(class_name_str);

        constexpr auto cls_size = static_cast<size_t>(page_align_up(sizeof(USER_CLASS)));
        const auto cls_ptr = c.win_emu.memory.allocate_memory(cls_size, memory_permission::read);

        const auto wnd_class = wnd_class_ex.read();

        c.proc.classes.emplace(class_name_str, process_context::class_entry{cls_ptr, wnd_class, class_menu_name.read()});

        return index;
    }

    NTSTATUS handle_NtUserUnregisterClass(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                          const emulator_pointer /*instance*/,
                                          const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name)
    {
        const auto cls_name = read_unicode_string(c.emu, class_name);

        if (const auto it = c.proc.classes.find(cls_name); it != c.proc.classes.end())
        {
            if (class_menu_name)
            {
                class_menu_name.write(it->second.menu_name);
            }

            c.win_emu.memory.release_memory(it->second.guest_obj_addr, 0);
            c.proc.classes.erase(it);
        }

        return c.proc.delete_atom(cls_name);
    }

    BOOL handle_NtUserGetClassInfoEx(const syscall_context& c, const hinstance /*instance*/,
                                     const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                     const emulator_object<EMU_WNDCLASSEX> wnd_class_ex, const emulator_pointer /*menu_name*/,
                                     const BOOL /*ansi*/)
    {
        std::u16string name_str = read_unicode_string(c.emu, class_name);

        auto it = c.proc.classes.find(name_str);
        if (it == c.proc.classes.end())
        {
            return FALSE;
        }

        if (wnd_class_ex)
        {
            wnd_class_ex.write(it->second.wnd_class);
        }

        return TRUE;
    }

    NTSTATUS handle_NtUserSetWindowsHookEx()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserUnhookWindowsHookEx()
    {
        return STATUS_NOT_SUPPORTED;
    }

    std::u16string read_large_string(const emulator_object<LARGE_STRING> str_obj)
    {
        if (!str_obj)
        {
            return {};
        }

        const auto str = str_obj.read();
        if (!str.bAnsi)
        {
            return read_string<char16_t>(*str_obj.get_memory_interface(), str.Buffer, str.Length / 2);
        }

        const auto ansi_string = read_string<char>(*str_obj.get_memory_interface(), str.Buffer, str.Length);
        return u8_to_u16(ansi_string);
    }

    hwnd handle_NtUserCreateWindowEx(const syscall_context& c, const DWORD ex_style, const emulator_object<LARGE_STRING> class_name,
                                     const emulator_object<LARGE_STRING> /*cls_version*/, const emulator_object<LARGE_STRING> window_name,
                                     const DWORD style, const int x, const int y, const int width, const int height, const hwnd /*parent*/,
                                     const hmenu /*menu*/, const hinstance /*instance*/, const pointer l_param, const DWORD /*flags*/,
                                     const pointer /*acbi_buffer*/)
    {
        const auto cls_name = read_large_string(class_name);
        const auto cls_it = c.proc.classes.find(cls_name);

        if (cls_it == c.proc.classes.end())
        {
            set_guest_last_error(c, 1407); // ERROR_CANNOT_FIND_WND_CLASS
            return 0;
        }

        const auto class_obj_addr = cls_it->second.guest_obj_addr;
        const auto* wnd_class = &cls_it->second.wnd_class;

        auto [handle, win] = c.proc.windows.create(c.win_emu.memory);
        win.ex_style = ex_style;
        win.style = style;
        win.x = x;
        win.y = y;
        win.width = width;
        win.height = height;
        win.thread_id = c.win_emu.current_thread().id;
        win.handle = handle.bits;
        win.class_name = cls_name;
        win.name = read_large_string(window_name);
        win.wnd_proc = wnd_class->lpfnWndProc;

        win.guest.access([&](USER_WINDOW& guest_win) {
            guest_win.hWnd = handle.bits;
            guest_win.ptrBase = win.guest.value();
            guest_win.dwExStyle = ex_style;
            guest_win.dwStyle = style;
            guest_win.lpfnWndProc = win.wnd_proc;
            guest_win.pcls = class_obj_addr;
            guest_win.cbWndExtra = wnd_class->cbWndExtra;

            if (wnd_class->cbWndExtra > 0)
            {
                const auto extra_size = static_cast<size_t>(page_align_up(wnd_class->cbWndExtra));
                guest_win.pExtraBytes = c.win_emu.memory.allocate_memory(extra_size, memory_permission::read);
            }
        });

        window_create_state state{};
        state.handle = handle.bits;

        EMU_CREATESTRUCT cs{};
        cs.lpCreateParams = l_param;
        state.create_struct_alloc = c.emu.push_stack(cs);

        RECT wr{};
        state.window_rect_alloc = c.emu.push_stack(wr);

        EMU_MINMAXINFO mmi{};
        state.min_max_info_alloc = c.emu.push_stack(mmi);

        state.message_queue = {
            {.message = WM_CREATE, .wParam = 0, .lParam = state.create_struct_alloc.address},
            {.message = WM_NCCALCSIZE, .wParam = 0, .lParam = state.window_rect_alloc.address},
            {.message = WM_NCCREATE, .wParam = 0, .lParam = state.create_struct_alloc.address},
            {.message = WM_GETMINMAXINFO, .wParam = 0, .lParam = state.min_max_info_alloc.address},
        };

        if ((style & WS_VISIBLE) != 0)
        {
            EMU_WINDOWPOS wp{};
            wp.hwnd = handle.bits;
            wp.hwndInsertAfter = 0;
            wp.x = x;
            wp.y = y;
            wp.cx = width;
            wp.cy = height;
            wp.flags = SWP_SHOWWINDOW;
            state.window_pos_alloc = c.emu.push_stack(wp);

            const auto move_lparam = static_cast<uint64_t>(((y & 0xFFFF) << 16) | (x & 0xFFFF));
            const auto size_lparam = static_cast<uint64_t>(((height & 0xFFFF) << 16) | (width & 0xFFFF));

            const std::initializer_list<qmsg> sw_messages = {
                {.message = WM_MOVE, .wParam = 0, .lParam = move_lparam},
                {.message = WM_SIZE, .wParam = 0, .lParam = size_lparam},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_SETFOCUS, .wParam = 0, .lParam = 0},
                {.message = WM_ACTIVATE, .wParam = 1, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = 1, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_SHOWWINDOW, .wParam = 1, .lParam = 0},
            };
            state.message_queue.insert(state.message_queue.begin(), sw_messages);
        }

        dispatch_next_message(c, callback_id::NtUserCreateWindowEx, std::move(state), win, state.message_queue);
        return {};
    }

    hwnd completion_NtUserCreateWindowEx(const syscall_context& c, const DWORD /*ex_style*/,
                                         const emulator_object<LARGE_STRING> /*class_name*/,
                                         const emulator_object<LARGE_STRING> /*cls_version*/,
                                         const emulator_object<LARGE_STRING> /*window_name*/, const DWORD /*style*/, const int /*x*/,
                                         const int /*y*/, const int /*width*/, const int /*height*/, const hwnd /*parent*/,
                                         const hmenu /*menu*/, const hinstance /*instance*/, const pointer /*l_param*/,
                                         const DWORD /*flags*/, const pointer /*acbi_buffer*/)
    {
        auto& s = c.get_completion_state<window_create_state>();
        const auto* win = c.proc.windows.get(s.handle);

        if (!s.message_queue.empty())
        {
            dispatch_next_message(c, callback_id::NtUserCreateWindowEx, std::move(s), *win, s.message_queue);
            return {};
        }

        if (s.window_pos_alloc.address != 0)
        {
            c.emu.pop_stack(std::move(s.window_pos_alloc));
        }

        c.emu.pop_stack(std::move(s.min_max_info_alloc));
        c.emu.pop_stack(std::move(s.window_rect_alloc));
        c.emu.pop_stack(std::move(s.create_struct_alloc));

        return s.handle;
    }

    BOOL handle_NtUserDestroyWindow(const syscall_context& c, const hwnd window)
    {
        auto* win = c.proc.windows.get(window);
        if (!win)
        {
            return FALSE;
        }

        if (win->thread_id != c.proc.active_thread->id)
        {
            return FALSE;
        }

        window_destroy_state state{};

        if ((win->style & WS_VISIBLE) != 0)
        {
            EMU_WINDOWPOS wp{};
            wp.hwnd = window;
            wp.hwndInsertAfter = 0;
            wp.x = win->x;
            wp.y = win->y;
            wp.cx = win->width;
            wp.cy = win->height;
            wp.flags = SWP_HIDEWINDOW;
            state.window_pos_alloc = c.emu.push_stack(wp);

            state.message_queue = {
                {.message = WM_NCDESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_KILLFOCUS, .wParam = 0, .lParam = 0},
                {.message = WM_ACTIVATE, .wParam = 0, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = FALSE, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
        }
        else
        {
            state.message_queue = {
                {.message = WM_NCDESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_DESTROY, .wParam = 0, .lParam = 0},
                {.message = WM_UAHDESTROYWINDOW, .wParam = 0, .lParam = 0},
            };
        }

        dispatch_next_message(c, callback_id::NtUserDestroyWindow, std::move(state), *win, state.message_queue);
        return {};
    }

    BOOL completion_NtUserDestroyWindow(const syscall_context& c, const hwnd window)
    {
        auto& s = c.get_completion_state<window_destroy_state>();
        auto* win = c.proc.windows.get(window);

        if (!s.message_queue.empty())
        {
            dispatch_next_message(c, callback_id::NtUserDestroyWindow, std::move(s), *win, s.message_queue);
            return {};
        }

        if (s.window_pos_alloc.address != 0)
        {
            c.emu.pop_stack(std::move(s.window_pos_alloc));
        }

        return c.proc.windows.erase(window);
    }

    BOOL handle_NtUserSetProp(const syscall_context& c, const hwnd window, const uint16_t atom, const uint64_t data)
    {
        auto* win = c.proc.windows.get(window);
        const auto* prop = c.proc.get_atom_name(atom);

        if (!win || !prop)
        {
            return FALSE;
        }

        win->props[*prop] = data;

        return TRUE;
    }

    BOOL handle_NtUserSetProp2(const syscall_context& c, const hwnd window,
                               const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str, const uint64_t data)
    {
        auto* win = c.proc.windows.get(window);
        if (!win || !str)
        {
            return FALSE;
        }

        auto prop = read_unicode_string(c.emu, str);
        win->props[std::move(prop)] = data;

        return TRUE;
    }

    uint64_t handle_NtUserChangeWindowMessageFilterEx()
    {
        return 0;
    }

    BOOL handle_NtUserShowWindow(const syscall_context& c, const hwnd hwnd, const LONG cmd_show)
    {
        auto* win = c.proc.windows.get(hwnd);
        if (!win)
        {
            return FALSE;
        }

        if (win->thread_id != c.proc.active_thread->id)
        {
            // TODO: Wait?
            return FALSE;
        }

        const bool want_visible = (cmd_show != 0); // SW_HIDE
        const bool was_visible = (win->style & WS_VISIBLE) != 0;

        if (want_visible == was_visible)
        {
            return was_visible ? TRUE : FALSE;
        }

        window_show_state state{};
        state.was_visible = was_visible;

        EMU_WINDOWPOS wp{};
        wp.hwnd = hwnd;
        wp.hwndInsertAfter = 0;
        wp.x = win->x;
        wp.y = win->y;
        wp.cx = win->width;
        wp.cy = win->height;
        wp.flags = want_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        state.window_pos_alloc = c.emu.push_stack(wp);

        if (want_visible)
        {
            const auto move_lparam = static_cast<uint64_t>(((win->y & 0xFFFF) << 16) | (win->x & 0xFFFF));
            const auto size_lparam = static_cast<uint64_t>(((win->height & 0xFFFF) << 16) | (win->width & 0xFFFF));

            state.message_queue = {
                {.message = WM_MOVE, .wParam = 0, .lParam = move_lparam},
                {.message = WM_SIZE, .wParam = 0, .lParam = size_lparam},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_SETFOCUS, .wParam = 0, .lParam = 0},
                {.message = WM_ACTIVATE, .wParam = 1, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = TRUE, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_SHOWWINDOW, .wParam = TRUE, .lParam = 0},
            };

            win->style |= WS_VISIBLE;
        }
        else
        {
            state.message_queue = {
                {.message = WM_KILLFOCUS, .wParam = 0, .lParam = 0},
                {.message = WM_ACTIVATE, .wParam = 0, .lParam = 0},
                {.message = WM_NCACTIVATE, .wParam = FALSE, .lParam = 0},
                {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address},
                {.message = WM_SHOWWINDOW, .wParam = FALSE, .lParam = 0},
            };

            win->style &= ~WS_VISIBLE;
        }

        win->guest.access([&](USER_WINDOW& guest_win) { //
            guest_win.dwStyle = win->style;
        });

        dispatch_next_message(c, callback_id::NtUserShowWindow, std::move(state), *win, state.message_queue);
        return {};
    }

    BOOL completion_NtUserShowWindow(const syscall_context& c, const hwnd hwnd, const LONG /*cmd_show*/)
    {
        auto& s = c.get_completion_state<window_show_state>();
        const auto* win = c.proc.windows.get(hwnd);

        if (!s.message_queue.empty())
        {
            dispatch_next_message(c, callback_id::NtUserShowWindow, std::move(s), *win, s.message_queue);
            return {};
        }

        c.emu.pop_stack(std::move(s.window_pos_alloc));

        return s.was_visible ? TRUE : FALSE;
    }

    BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd, const UINT msg_filter_min,
                                 const UINT msg_filter_max)
    {
        auto& t = c.win_emu.current_thread();

        if (auto pending_msg = t.peek_pending_message(hwnd, msg_filter_min, msg_filter_max, true))
        {
            message.write(*pending_msg);
            return pending_msg->message != WM_QUIT ? TRUE : FALSE;
        }

        t.await_msg = {message, hwnd, msg_filter_min, msg_filter_max};

        c.win_emu.yield_thread(false);
        return {};
    }

    BOOL handle_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd, const UINT msg_filter_min,
                                  const UINT msg_filter_max, const UINT remove_message)
    {
        auto& t = c.win_emu.current_thread();

        const bool should_remove = (remove_message & PM_REMOVE) != 0;
        std::optional<msg> pending_msg = t.peek_pending_message(hwnd, msg_filter_min, msg_filter_max, should_remove);

        if (pending_msg)
        {
            message.write(*pending_msg);
            return TRUE;
        }

        return FALSE;
    }

    BOOL handle_NtUserPostMessage(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t wParam, const uint64_t lParam)
    {
        const auto* win = c.proc.windows.get(hwnd);
        if (!win && hwnd != 0)
        {
            return FALSE;
        }

        uint32_t target_thread_id = hwnd != 0 ? win->thread_id : c.win_emu.current_thread().id;

        for (auto& thread : c.proc.threads | std::views::values)
        {
            if (thread.id == target_thread_id)
            {
                ::msg qmsg{};
                qmsg.window = hwnd;
                qmsg.message = msg;
                qmsg.wParam = wParam;
                qmsg.lParam = lParam;

                thread.post_message(qmsg);
                return TRUE;
            }
        }

        return FALSE;
    }

    BOOL handle_NtUserPostQuitMessage(const syscall_context& c, int exit_code)
    {
        msg qmsg{};
        qmsg.message = WM_QUIT;
        qmsg.wParam = exit_code;

        c.proc.active_thread->post_message(qmsg);
        return TRUE;
    }

    NTSTATUS handle_NtUserEnumDisplayDevices(const syscall_context& c,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str_device, const DWORD dev_num,
                                             const emulator_object<EMU_DISPLAY_DEVICEW> display_device, const DWORD /*flags*/)
    {
        if (!str_device)
        {
            if (dev_num > 0)
            {
                return STATUS_UNSUCCESSFUL;
            }

            display_device.access([&](EMU_DISPLAY_DEVICEW& dev) {
                dev.StateFlags = 0x5; // DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
                utils::string::copy(dev.DeviceName, u"\\\\.\\DISPLAY1");
                utils::string::copy(dev.DeviceString, u"Emulated Virtual Adapter");
                utils::string::copy(dev.DeviceID, u"PCI\\VEN_10DE&DEV_0000&SUBSYS_00000000&REV_A1");
                utils::string::copy(dev.DeviceKey, u"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Video\\{00000001-"
                                                   u"0002-0003-0004-000000000005}\\0000");
            });
        }
        else
        {
            const auto dev_name = read_unicode_string(c.emu, str_device);

            if (dev_name != u"\\\\.\\DISPLAY1")
            {
                return STATUS_UNSUCCESSFUL;
            }

            if (dev_num > 0)
            {
                return STATUS_UNSUCCESSFUL;
            }

            display_device.access([&](EMU_DISPLAY_DEVICEW& dev) {
                dev.StateFlags = 0x1; // DISPLAY_DEVICE_ACTIVE
                utils::string::copy(dev.DeviceName, u"\\\\.\\DISPLAY1\\Monitor0");
                utils::string::copy(dev.DeviceString, u"Generic PnP Monitor");
                utils::string::copy(dev.DeviceID, u"MONITOR\\EMU1234\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\0000");
                utils::string::copy(dev.DeviceKey, u"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\DISPLAY\\EMU1234\\"
                                                   u"1&23a45b&0&UID67568640");
            });
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserEnumDisplaySettings(const syscall_context& c,
                                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name,
                                              const DWORD mode_num, const emulator_object<EMU_DEVMODEW> dev_mode, const DWORD /*flags*/)
    {
        if (dev_mode && (mode_num == ENUM_CURRENT_SETTINGS || mode_num == 0))
        {
            const auto dev_name = read_unicode_string(c.emu, device_name);

            if (dev_name == u"\\\\.\\DISPLAY1")
            {
                dev_mode.access([](EMU_DEVMODEW& dm) {
                    dm.dmFields = 0x5C0000; // DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
                    dm.dmPelsWidth = 1920;
                    dm.dmPelsHeight = 1080;
                    dm.dmBitsPerPel = 32;
                    dm.dmDisplayFrequency = 60;
                });

                return STATUS_SUCCESS;
            }
        }

        return STATUS_UNSUCCESSFUL;
    }

    BOOL handle_NtUserEnumDisplayMonitors(const syscall_context& c, const hdc hdc_in, const uint64_t clip_rect_ptr, const uint64_t callback,
                                          const uint64_t param)
    {
        if (!callback)
        {
            return FALSE;
        }

        const auto hmon = c.win_emu.process.default_monitor_handle.bits;
        const auto display_info = c.proc.user_handles.get_display_info().read();

        if (clip_rect_ptr)
        {
            RECT clip{};
            c.emu.read_memory(clip_rect_ptr, &clip, sizeof(clip));

            const emulator_object<USER_MONITOR> monitor_obj(c.emu, display_info.pPrimaryMonitor);
            const auto monitor = monitor_obj.read();

            auto effective_rc{monitor.rcMonitor};
            effective_rc.left = std::max(effective_rc.left, clip.left);
            effective_rc.top = std::max(effective_rc.top, clip.top);
            effective_rc.right = std::min(effective_rc.right, clip.right);
            effective_rc.bottom = std::min(effective_rc.bottom, clip.bottom);
            if (effective_rc.right <= effective_rc.left || effective_rc.bottom <= effective_rc.top)
            {
                return TRUE;
            }
        }

        const uint64_t rect_ptr = display_info.pPrimaryMonitor + offsetof(USER_MONITOR, rcMonitor);
        dispatch_user_callback(c, callback_id::NtUserEnumDisplayMonitors, callback, hmon, hdc_in, rect_ptr, param);
        return {};
    }

    BOOL completion_NtUserEnumDisplayMonitors(const syscall_context& c, const hdc /*hdc_in*/, const uint64_t /*clip_rect_ptr*/,
                                              const uint64_t /*callback*/, const uint64_t /*param*/)
    {
        return c.get_callback_result<BOOL>();
    }

    BOOL handle_NtUserGetHDevName(const syscall_context& c, handle hdev, emulator_pointer device_name)
    {
        if (hdev != c.proc.default_monitor_handle)
        {
            return FALSE;
        }

        const std::u16string name = u"\\\\.\\DISPLAY1";
        c.emu.write_memory(device_name, name.c_str(), (name.size() + 1) * sizeof(char16_t));

        return TRUE;
    }

    emulator_pointer handle_NtUserMapDesktopObject(const syscall_context& c, handle handle)
    {
        const auto index = handle.value.id;

        if (index == 0 || index >= user_handle_table::MAX_HANDLES)
        {
            return 0;
        }

        const auto handle_entry = c.proc.user_handles.get_handle_table().read(static_cast<size_t>(index));
        return handle_entry.pHead;
    }

    NTSTATUS handle_NtUserTransformRect()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserSetWindowPos()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserSetForegroundWindow()
    {
        return STATUS_SUCCESS;
    }

    emulator_pointer handle_NtUserSetWindowLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                   BOOL /*Ansi*/)
    {
        auto* win = c.proc.windows.get(hWnd);
        if (!win)
        {
            return 0;
        }

        emulator_pointer oldValue = 0;

        win->guest.access([&](USER_WINDOW& guest_win) {
            if (nIndex >= 0)
            {
                const auto offsetCorrection = guest_win.wndExtraOffset;
                const auto pBaseExtraBytes = guest_win.pExtraBytes;

                if (pBaseExtraBytes == 0)
                {
                    return;
                }

                const auto targetAddress = pBaseExtraBytes + (nIndex - offsetCorrection);

                c.win_emu.memory.read_memory(targetAddress, &oldValue, sizeof(oldValue));
                c.win_emu.memory.write_memory(targetAddress, &dwNewLong, sizeof(dwNewLong));
            }
            else
            {
                switch (nIndex)
                {
                case GWLP_USERDATA:
                    oldValue = guest_win.userData;
                    guest_win.userData = dwNewLong;
                    break;

                case GWLP_WNDPROC:
                    oldValue = guest_win.lpfnWndProc;
                    guest_win.lpfnWndProc = dwNewLong;
                    break;

                default:
                    break;
                }
            }
        });

        return oldValue;
    }

    uint32_t handle_NtUserSetWindowLong(const syscall_context& c, handle hWnd, int nIndex, uint32_t dwNewLong, BOOL Ansi)
    {
        const auto oldValue = handle_NtUserSetWindowLongPtr(c, hWnd, nIndex, static_cast<emulator_pointer>(dwNewLong), Ansi);
        return static_cast<uint32_t>(oldValue);
    }

    NTSTATUS handle_NtUserRedrawWindow()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserGetCPD()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserSetWindowFNID()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserEnableWindow()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserGetSystemMenu()
    {
        return STATUS_SUCCESS;
    }
}
