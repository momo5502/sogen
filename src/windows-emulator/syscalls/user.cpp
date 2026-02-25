#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../win32k_userconnect.hpp"
#include "common/segment_utils.hpp"
#include "windows-emulator/user_callback_dispatch.hpp"
#include <limits>
#include <utils/string.hpp>

namespace
{
    constexpr ULONG k_thread_state_win32_thread_info = 0xE;
    constexpr size_t k_win32_thread_info_slab_size = 0x2000;
    constexpr uint64_t k_win32_thread_info_bias = 0x800;
    // callback id used by wow64.dll!Wow64KiUserCallbackDispatcher
    constexpr uint32_t k_wow64_client_setup_callback_id = 0x54;
    // user32 callback id for ___ClientMonitorEnumProc@4
    constexpr uint32_t k_wow64_enum_display_monitors_callback_id = 0x57;
    constexpr size_t k_wow64_callback_context_buffer_size = 0x140;

    struct wow64_enum_display_monitors_callback_args
    {
        uint32_t monitor{};
        uint32_t dc{};
        RECT rect{};
        uint32_t param{};
        uint32_t callback{};
    };
    static_assert(offsetof(wow64_enum_display_monitors_callback_args, monitor) == 0x0);
    static_assert(offsetof(wow64_enum_display_monitors_callback_args, dc) == 0x4);
    static_assert(offsetof(wow64_enum_display_monitors_callback_args, rect) == 0x8);
    static_assert(offsetof(wow64_enum_display_monitors_callback_args, param) == 0x18);
    static_assert(offsetof(wow64_enum_display_monitors_callback_args, callback) == 0x1C);
    static_assert(sizeof(wow64_enum_display_monitors_callback_args) == 0x20);

    void set_guest_last_error(const syscall_context& c, uint32_t last_error)
    {
        c.proc.active_thread->teb64->access([&](TEB64& teb) {
            teb.LastErrorValue = static_cast<ULONG>(last_error); //
        });
    }

    void set_thread_window_context(const syscall_context& c, const uint64_t active_handle, const uint64_t active_window_ptr)
    {
        if (c.proc.active_thread && c.proc.active_thread->teb64)
        {
            c.proc.active_thread->teb64->access([&](TEB64& teb) {
                teb.Win32ClientInfo.arr[8] = active_handle;
                teb.Win32ClientInfo.arr[9] = active_window_ptr;
            });
        }

        if (c.proc.is_wow64_process && c.proc.active_thread && c.proc.active_thread->teb32)
        {
            uint32_t active_handle32{};
            uint32_t active_window_ptr32{};

            if (active_handle <= std::numeric_limits<uint32_t>::max())
            {
                active_handle32 = static_cast<uint32_t>(active_handle);
            }

            if (active_window_ptr <= std::numeric_limits<uint32_t>::max())
            {
                active_window_ptr32 = static_cast<uint32_t>(active_window_ptr);
            }

            c.proc.active_thread->teb32->access([&](TEB32& teb) {
                teb.Win32ClientInfo[8] = active_handle32;
                teb.Win32ClientInfo[9] = active_window_ptr32;
            });
        }
    }

    void set_thread_window_context(const syscall_context& c, const hwnd hwnd)
    {
        const auto* win = c.proc.windows.get(hwnd);

        uint64_t active_handle{};
        uint64_t active_window_ptr{};

        if (win)
        {
            active_handle = win->handle;
            active_window_ptr = win->guest.value();
        }

        set_thread_window_context(c, active_handle, active_window_ptr);
    }

    void set_user_handle_owner(const syscall_context& c, const handle h, const uint64_t owner)
    {
        if (owner == 0)
        {
            return;
        }

        const auto index = static_cast<uint32_t>(h.value.id);
        if (index == 0 || index >= user_handle_table::MAX_HANDLES)
        {
            return;
        }

        c.proc.user_handles.get_handle_table().access([&](USER_HANDLEENTRY& entry) { entry.pOwner = owner; }, index);
    }

    template <typename T>
    void dispatch_window_message(const syscall_context& c, callback_id id, T&& state, const window& win, uint32_t message,
                                 uint64_t w_param = 0, uint64_t l_param = 0)
    {
        set_thread_window_context(c, win.handle, win.guest.value());

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

    uint64_t ensure_win32_thread_info(const syscall_context& c)
    {
        auto* thread = c.proc.active_thread;
        if (!thread || !thread->teb64)
        {
            return 0;
        }

        if (thread->win32k_thread_info != 0)
        {
            return thread->win32k_thread_info;
        }

        uint64_t thread_info{};
        thread->teb64->access([&](const TEB64& teb) { thread_info = teb.Win32ThreadInfo; });

        if (thread_info != 0)
        {
            thread->win32k_thread_info = thread_info;
            return thread_info;
        }

        const auto slab_base = c.proc.base_allocator.reserve(k_win32_thread_info_slab_size, 0x10);
        std::vector<std::byte> zero_slab(k_win32_thread_info_slab_size);
        c.emu.write_memory(slab_base, zero_slab.data(), zero_slab.size());
        thread->win32k_thread_info = slab_base + k_win32_thread_info_bias;
        return thread->win32k_thread_info;
    }

    void publish_win32_thread_info(const syscall_context& c, const uint64_t thread_info)
    {
        auto* thread = c.proc.active_thread;
        if (!thread || !thread->teb64 || thread_info == 0)
        {
            return;
        }

        const auto low = static_cast<ULONG>(thread_info & 0xFFFFFFFFull);
        const auto high = static_cast<ULONG>((thread_info >> 32) & 0xFFFFFFFFull);

        thread->teb64->access([&](TEB64& teb64) {
            teb64.Win32ThreadInfo = thread_info;
            teb64.User32Reserved.arr[13] = low;
            teb64.User32Reserved.arr[14] = high;
        });

        if (c.proc.is_wow64_process && thread->teb32)
        {
            thread->teb32->access([&](TEB32& teb32) {
                teb32.Win32ThreadInfo = low;
                teb32.User32Reserved[13] = low;
                teb32.User32Reserved[14] = high;
            });
        }
    }

    NTSTATUS ensure_user_shared_info_ptr(const syscall_context& c, uint64_t& user_shared_info_ptr)
    {
        user_shared_info_ptr = 0;

        if (!c.proc.peb32)
        {
            return STATUS_SUCCESS;
        }

        c.proc.peb32->access([&](const PEB32& peb) { user_shared_info_ptr = peb.UserSharedInfoPtr; });

        if (user_shared_info_ptr != 0)
        {
            return STATUS_SUCCESS;
        }

        user_shared_info_ptr = c.proc.base_allocator.reserve(sizeof(WIN32K_USERCONNECT32), alignof(WIN32K_USERCONNECT32));
        std::array<std::byte, sizeof(WIN32K_USERCONNECT32)> zeros{};
        c.emu.write_memory(user_shared_info_ptr, zeros.data(), zeros.size());

        uint32_t user_shared_info_ptr32{};
        const auto narrow_status = win32k_userconnect::narrow_wow64_address(user_shared_info_ptr, user_shared_info_ptr32);
        if (narrow_status != STATUS_SUCCESS)
        {
            return narrow_status;
        }

        c.proc.peb32->access([&](PEB32& peb) { peb.UserSharedInfoPtr = user_shared_info_ptr32; });
        return STATUS_SUCCESS;
    }

    uint64_t resolve_wow64_callback_dispatcher(const syscall_context& c)
    {
        if (c.proc.wow64_ki_user_callback_dispatcher != 0)
        {
            return c.proc.wow64_ki_user_callback_dispatcher;
        }

        for (const auto& [_, mod] : c.win_emu.mod_manager.modules())
        {
            if (!utils::string::equals_ignore_case(std::string_view{mod.name}, std::string_view{"wow64.dll"}))
            {
                continue;
            }

            const auto dispatcher = mod.find_export("Wow64KiUserCallbackDispatcher");
            if (dispatcher != 0)
            {
                c.proc.wow64_ki_user_callback_dispatcher = dispatcher;
                return dispatcher;
            }
        }

        return 0;
    }

    uint64_t ensure_wow64_callback_buffer(const syscall_context& c, emulator_thread& thread)
    {
        if (thread.win32k_callback_buffer != 0)
        {
            return thread.win32k_callback_buffer;
        }

        thread.win32k_callback_buffer = c.proc.base_allocator.reserve(k_wow64_callback_context_buffer_size, 0x10);
        std::array<std::byte, k_wow64_callback_context_buffer_size> zeros{};
        c.emu.write_memory(thread.win32k_callback_buffer, zeros.data(), zeros.size());
        return thread.win32k_callback_buffer;
    }

    bool schedule_wow64_callback(const syscall_context& c, emulator_thread& thread, const uint32_t callback_id, const uint64_t arg_buffer,
                                 const uint32_t arg_length, const std::optional<pending_wow64_callback>& pending_callback = std::nullopt)
    {
        if (!c.proc.is_wow64_process)
        {
            return false;
        }

        thread.win32k_pending_wow64_callback.reset();

        const auto dispatcher = resolve_wow64_callback_dispatcher(c);
        if (dispatcher == 0)
        {
            return false;
        }

        const auto cs_selector = c.emu.reg<uint16_t>(x86_register::cs);
        const auto bitness = segment_utils::get_segment_bitness(c.emu, cs_selector);
        if (!bitness || *bitness != segment_utils::segment_bitness::bit64)
        {
            return false;
        }

        const auto callback_buffer = ensure_wow64_callback_buffer(c, thread);
        std::array<std::byte, k_wow64_callback_context_buffer_size> zeros{};
        c.emu.write_memory(callback_buffer, zeros.data(), zeros.size());

        c.emu.reg(x86_register::rcx, callback_buffer);
        c.emu.reg(x86_register::rdx, static_cast<uint64_t>(callback_id));
        c.emu.reg(x86_register::r8, arg_buffer);
        c.emu.reg(x86_register::r9, static_cast<uint64_t>(arg_length));
        c.emu.reg(x86_register::rip, dispatcher);
        thread.win32k_pending_wow64_callback = pending_callback;
        return true;
    }

    bool schedule_wow64_client_callback(const syscall_context& c, emulator_thread& thread)
    {
        return schedule_wow64_callback(c, thread, k_wow64_client_setup_callback_id, 0, 0);
    }

}

namespace syscalls
{
    hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, handle bitmap);

    NTSTATUS handle_NtUserTraceLoggingSendMixedModeTelemetry()
    {
        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserRegisterWindowMessage()
    {
        return STATUS_NOT_SUPPORTED;
    }

    NTSTATUS handle_NtUserGetThreadState(const syscall_context& c, const ULONG routine)
    {
        if (routine != k_thread_state_win32_thread_info)
        {
            return STATUS_SUCCESS;
        }

        const auto thread_info = ensure_win32_thread_info(c);
        if (thread_info == 0)
        {
            return STATUS_UNSUCCESSFUL;
        }

        publish_win32_thread_info(c, thread_info);

        if (c.proc.is_wow64_process && c.proc.active_thread && !c.proc.active_thread->win32k_thread_setup_done &&
            !c.proc.active_thread->win32k_thread_setup_pending)
        {
            if (!schedule_wow64_client_callback(c, *c.proc.active_thread))
            {
                return STATUS_UNSUCCESSFUL;
            }

            c.proc.active_thread->win32k_thread_setup_pending = true;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserProcessConnect(const syscall_context& c, const handle process_handle, const ULONG length,
                                         const emulator_pointer user_connect)
    {
        if (!c.proc.is_wow64_process)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (process_handle != CURRENT_PROCESS)
        {
            return STATUS_INVALID_HANDLE;
        }

        uint32_t connect_destination{};
        const auto destination_status = win32k_userconnect::resolve_wow64_destination(user_connect, length, connect_destination);
        if (destination_status != STATUS_SUCCESS)
        {
            return destination_status;
        }

        WIN32K_USERCONNECT32 connect_info{};
        const auto connect_status = win32k_userconnect::build_wow64_userconnect(c.proc, connect_info);
        if (connect_status != STATUS_SUCCESS)
        {
            return connect_status;
        }

        if (!win32k_userconnect::try_write_wow64_userconnect(c.emu, connect_destination, connect_info))
        {
            return STATUS_INVALID_PARAMETER;
        }

        uint64_t user_shared_info_ptr{};
        const auto shared_info_status = ensure_user_shared_info_ptr(c, user_shared_info_ptr);
        if (shared_info_status != STATUS_SUCCESS)
        {
            return shared_info_status;
        }

        if (user_shared_info_ptr != 0)
        {
            if (!win32k_userconnect::try_write_wow64_userconnect(c.emu, user_shared_info_ptr, connect_info))
            {
                return STATUS_INVALID_PARAMETER;
            }
        }

        if (c.proc.active_thread)
        {
            c.proc.active_thread->win32k_thread_setup_pending = false;
            c.proc.active_thread->win32k_thread_setup_done = true;
        }

        return STATUS_SUCCESS;
    }

    NTSTATUS handle_NtUserInitializeClientPfnArrays(const syscall_context& c, const emulator_pointer apfn_client_a,
                                                    const emulator_pointer apfn_client_w, const emulator_pointer apfn_client_worker,
                                                    const emulator_pointer /*hmod_user*/)
    {
        if (c.proc.active_thread)
        {
            c.proc.active_thread->win32k_thread_setup_pending = false;
            c.proc.active_thread->win32k_thread_setup_done = true;
        }

        if (!win32k_userconnect::try_update_client_pfn_arrays_from_addresses(c.win_emu.memory, c.proc, apfn_client_a, apfn_client_w,
                                                                             apfn_client_worker))
        {
            return STATUS_UNSUCCESSFUL;
        }

        return STATUS_SUCCESS;
    }

    uint64_t handle_NtUserRemoteConnectState(const syscall_context&)
    {
        return 1;
    }

    hdesk handle_NtUserGetThreadDesktop(const syscall_context& c, const ULONG thread_id)
    {
        emulator_thread* target = nullptr;
        if (thread_id == 0 || (c.proc.active_thread && c.proc.active_thread->id == thread_id))
        {
            target = c.proc.active_thread;
        }
        else
        {
            for (auto& [_, thread] : c.proc.threads)
            {
                if (thread.id == thread_id)
                {
                    target = &thread;
                    break;
                }
            }
        }

        if (!target)
        {
            return 0;
        }

        if (target->win32k_desktop.bits == 0)
        {
            if (c.proc.default_desktop.bits == 0)
            {
                desktop desk{};
                desk.name = u"Default";
                c.proc.default_desktop = c.proc.desktops.store(std::move(desk));
            }

            target->win32k_desktop = c.proc.default_desktop;
        }

        return target->win32k_desktop.bits;
    }

    hdc handle_NtUserGetDCEx(const syscall_context& c, const hwnd /*window*/, const uint64_t /*clip_region*/, const ULONG /*flags*/)
    {
        return handle_NtGdiGetDCforBitmap(c, {});
    }

    hdc handle_NtUserGetDC(const syscall_context& c, const hwnd window)
    {
        return handle_NtUserGetDCEx(c, window, 0, 0);
    }

    hdc handle_NtUserGetWindowDC(const syscall_context& c, const hwnd /*window*/)
    {
        return handle_NtGdiGetDCforBitmap(c, {});
    }

    BOOL handle_NtUserReleaseDC()
    {
        return TRUE;
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

        const auto thread_info = ensure_win32_thread_info(c);
        publish_win32_thread_info(c, thread_info);
        set_user_handle_owner(c, handle, thread_info);

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

    uint64_t handle_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t w_param,
                                      const uint64_t l_param, const uint64_t /*result_info*/, const DWORD /*type*/, const BOOL /*ansi*/)
    {
        const auto* win = c.proc.windows.get(hwnd);
        if (!win)
        {
            return 0;
        }

        if (win->thread_id != c.proc.active_thread->id)
        {
            return 0;
        }

        dispatch_window_message(c, callback_id::NtUserMessageCall, nullptr, *win, msg, w_param, l_param);
        return {};
    }

    uint64_t completion_NtUserMessageCall(const syscall_context& c, const hwnd /*hwnd*/, const UINT /*msg*/, const uint64_t /*w_param*/,
                                          const uint64_t /*l_param*/, const uint64_t /*result_info*/, const DWORD /*type*/,
                                          const BOOL /*ansi*/)
    {
        return c.get_callback_result<uint64_t>();
    }

    BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd, const UINT msg_filter_min,
                                 const UINT msg_filter_max)
    {
        auto& t = c.win_emu.current_thread();

        if (auto pending_msg = t.peek_pending_message(hwnd, msg_filter_min, msg_filter_max, true))
        {
            message.write(*pending_msg);
            set_thread_window_context(c, pending_msg->window);
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
            set_thread_window_context(c, pending_msg->window);
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
        const emulator_object<USER_MONITOR> monitor_obj(c.emu, display_info.pPrimaryMonitor);
        if (!monitor_obj)
        {
            return FALSE;
        }

        const auto monitor = monitor_obj.read();
        auto effective_rc = monitor.rcMonitor;

        if (clip_rect_ptr)
        {
            RECT clip{};
            c.emu.read_memory(clip_rect_ptr, &clip, sizeof(clip));

            effective_rc.left = std::max(effective_rc.left, clip.left);
            effective_rc.top = std::max(effective_rc.top, clip.top);
            effective_rc.right = std::min(effective_rc.right, clip.right);
            effective_rc.bottom = std::min(effective_rc.bottom, clip.bottom);
            if (effective_rc.right <= effective_rc.left || effective_rc.bottom <= effective_rc.top)
            {
                return TRUE;
            }
        }

        if (c.proc.is_wow64_process)
        {
            if (!c.proc.active_thread)
            {
                return FALSE;
            }

            if (hmon > std::numeric_limits<uint32_t>::max() || hdc_in > std::numeric_limits<uint32_t>::max() ||
                callback > std::numeric_limits<uint32_t>::max() || param > std::numeric_limits<uint32_t>::max())
            {
                return FALSE;
            }

            wow64_enum_display_monitors_callback_args args{};
            args.monitor = static_cast<uint32_t>(hmon);
            args.dc = static_cast<uint32_t>(hdc_in);
            args.param = static_cast<uint32_t>(param);
            args.callback = static_cast<uint32_t>(callback);
            args.rect = effective_rc;

            const auto arg_buffer = c.proc.base_allocator.reserve(sizeof(args), alignof(uint32_t));
            emulator_object<wow64_enum_display_monitors_callback_args>{c.emu, arg_buffer}.write(args);

            pending_wow64_callback pending_callback{};
            pending_callback.callback_id = k_wow64_enum_display_monitors_callback_id;
            pending_callback.postprocess = wow64_callback_postprocess::bool_result_to_status;

            if (!schedule_wow64_callback(c, *c.proc.active_thread, k_wow64_enum_display_monitors_callback_id, arg_buffer,
                                         static_cast<uint32_t>(sizeof(args)), pending_callback))
            {
                return FALSE;
            }

            return TRUE;
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
        if (handle.value.type == handle_types::desktop && !handle.value.is_pseudo)
        {
            auto* desktop = c.proc.desktops.get(handle);
            if (!desktop)
            {
                return 0;
            }

            if (desktop->mapped_object == 0)
            {
                desktop->mapped_object = c.proc.base_allocator.reserve(sizeof(USER_DESKTOPINFO), alignof(USER_DESKTOPINFO));
                std::array<std::byte, sizeof(USER_DESKTOPINFO)> zeros{};
                c.emu.write_memory(desktop->mapped_object, zeros.data(), zeros.size());
            }

            return desktop->mapped_object;
        }

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
