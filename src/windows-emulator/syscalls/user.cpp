#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../win32k_userconnect.hpp"
#include "windows-emulator/user_callback_dispatch.hpp"
#include <limits>

#ifdef msg
#undef msg
#endif

namespace sogen
{

    namespace
    {
        constexpr ULONG k_thread_state_win32_thread_info = 0xE;
        constexpr size_t k_win32_thread_info_slab_size = 0x2000;
        constexpr uint64_t k_win32_thread_info_bias = 0x800;
        constexpr uint32_t k_client_setup_callback_id = 0x54;
        constexpr uint32_t k_enum_display_monitors_callback_id = 0x57;
        constexpr uint32_t k_fn_dword_callback_id = 0x02;
        constexpr uint32_t k_fn_nc_destroy_callback_id = 0x03;
        constexpr uint32_t k_fn_in_lp_create_struct_callback_id = 0x0A;
        constexpr uint32_t k_fn_in_lp_window_pos_callback_id = 0x11;
        constexpr uint32_t k_fn_inout_lp_point5_callback_id = 0x12;
        constexpr uint32_t k_fn_inout_nc_calc_size_callback_id = 0x15;

        struct user_callback_capture_buffer
        {
            DWORD cbCallback{};
            DWORD cbCapture{};
            DWORD cCapturedPointers{};
            pointer pbFree{};
            DWORD offPointers{};
            pointer pvVirtualAddress{};
        };

        struct fn_dword_message
        {
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            lparam lParam{};
            pointer xParam{};
            pointer xpfnProc{};
        };

        struct fn_in_lp_create_struct_message
        {
            user_callback_capture_buffer captureBuffer{};
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            lparam lParam{};
            EMU_CREATESTRUCT cs{};
            pointer xParam{};
            pointer xpfnProc{};
        };

        struct fn_in_lp_window_pos_message
        {
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            EMU_WINDOWPOS wp{};
            pointer xParam{};
            pointer xpfnProc{};
        };

        struct fn_inout_lp_point5_message
        {
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            union
            {
                EMU_MINMAXINFO point5;
                EMU_WINDOWPOS window_pos;
            } data{};
            pointer xParam{};
            pointer xpfnProc{};
        };

        struct EMU_NCCALCSIZE_PARAMS
        {
            std::array<RECT, 3> rgrc{};
            pointer lppos{};
        };

        struct fn_inout_nc_calc_size_message
        {
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            pointer xParam{};
            pointer xpfnProc{};
            union
            {
                RECT rect;
                struct
                {
                    EMU_NCCALCSIZE_PARAMS params;
                    EMU_WINDOWPOS pos;
                } data;
            } u{};
        };

        struct enum_display_monitors_callback_args
        {
            hdc monitor{};
            hdc dc{};
            RECT rect{};
            pointer data{};
            pointer callback{};
        };

        void set_guest_last_error(const syscall_context& c, uint32_t last_error)
        {
            c.proc.active_thread->teb64->access([&](TEB64& teb) {
                teb.LastErrorValue = static_cast<ULONG>(last_error); //
            });
        }

        std::u16string make_atom_class_name(const uint16_t atom)
        {
            std::u16string name = u"#";
            for (const char ch : std::to_string(atom))
            {
                name.push_back(static_cast<char16_t>(ch));
            }

            return name;
        }

        std::u16string_view normalize_builtin_window_class_name(const std::u16string_view class_name)
        {
            if (class_name == u"#1" || class_name == u"BUTTON" || class_name == u"Button")
            {
                return u"Button";
            }
            if (class_name == u"#2" || class_name == u"EDIT" || class_name == u"Edit")
            {
                return u"Edit";
            }
            if (class_name == u"#3" || class_name == u"#2160" || class_name == u"STATIC" || class_name == u"Static")
            {
                return u"Static";
            }
            if (class_name == u"#4" || class_name == u"LISTBOX" || class_name == u"ListBox")
            {
                return u"ListBox";
            }
            if (class_name == u"#5" || class_name == u"SCROLLBAR" || class_name == u"ScrollBar")
            {
                return u"ScrollBar";
            }
            if (class_name == u"#6" || class_name == u"COMBOBOX" || class_name == u"ComboBox")
            {
                return u"ComboBox";
            }

            return class_name;
        }

        bool is_builtin_window_class_name(const std::u16string_view class_name)
        {
            const auto normalized = normalize_builtin_window_class_name(class_name);
            return normalized == u"#32770" || normalized == u"Button" || normalized == u"Static";
        }

        process_context::class_entry* ensure_builtin_window_class(const syscall_context& c, const std::u16string_view class_name)
        {
            const auto normalized_name = normalize_builtin_window_class_name(class_name);

            const auto it = c.proc.classes.find(class_name);
            if (it != c.proc.classes.end())
            {
                return &it->second;
            }

            if (class_name != normalized_name)
            {
                if (const auto normalized_it = c.proc.classes.find(normalized_name); normalized_it != c.proc.classes.end())
                {
                    c.proc.classes.insert_or_assign(std::u16string{class_name}, normalized_it->second);
                    return &c.proc.classes.find(class_name)->second;
                }
            }

            if (!is_builtin_window_class_name(normalized_name))
            {
                return nullptr;
            }

            uint64_t wnd_proc = 0;
            int wnd_extra = 0;

            if (!c.win_emu.mod_manager.ntdll)
            {
                return nullptr;
            }

            if (normalized_name == u"#32770")
            {
                wnd_proc = c.win_emu.mod_manager.ntdll->find_export("NtdllDialogWndProc_W");
                wnd_extra = 30; // DLGWINDOWEXTRA
            }
            else
            {
                wnd_proc = c.win_emu.mod_manager.ntdll->find_export("NtdllDefWindowProc_A");
            }

            if (wnd_proc == 0)
            {
                return nullptr;
            }

            constexpr auto cls_size = static_cast<size_t>(page_align_up(sizeof(USER_CLASS)));
            const auto cls_ptr = c.win_emu.memory.allocate_memory(cls_size, memory_permission::read);

            EMU_WNDCLASSEX wnd_class{};
            wnd_class.cbSize = sizeof(wnd_class);
            wnd_class.lpfnWndProc = wnd_proc;
            wnd_class.cbWndExtra = wnd_extra;

            const auto entry = process_context::class_entry{cls_ptr, wnd_class, {}};
            c.proc.classes.insert_or_assign(std::u16string{normalized_name}, entry);
            c.proc.classes.insert_or_assign(std::u16string{class_name}, entry);
            return &c.proc.classes.find(class_name)->second;
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

        void invalidate_window(const syscall_context& c, window& win, const std::optional<RECT>& update_rect, bool erase);

        RECT get_client_rect(const window& win)
        {
            return RECT{.left = 0, .top = 0, .right = win.width, .bottom = win.height};
        }

        RECT get_window_rect(const window& win)
        {
            return RECT{.left = win.x, .top = win.y, .right = win.x + win.width, .bottom = win.y + win.height};
        }

        void sync_guest_window_rects(window& win)
        {
            const auto window_rect = get_window_rect(win);

            win.guest.access([&](USER_WINDOW& guest_win) {
                guest_win.rcUnknown_048 = window_rect;
                guest_win.rcWindow = window_rect;
                guest_win.rcClient = window_rect;
            });
        }

        void update_window_geometry(const syscall_context& c, window& win, const int x, const int y, const int width, const int height,
                                    const bool repaint)
        {
            win.x = x;
            win.y = y;
            win.width = width;
            win.height = height;
            sync_guest_window_rects(win);

            if (win.host_surface_window)
            {
                c.win_emu.ui().set_window_rect(win.handle, get_window_rect(win));
            }

            if (repaint)
            {
                invalidate_window(c, win, std::nullopt, false);
            }
        }

        void queue_window_paint(const syscall_context& c, window& win)
        {
            if (win.paint_message_posted)
            {
                return;
            }

            for (auto& thread : c.proc.threads | std::views::values)
            {
                if (thread.id != win.thread_id)
                {
                    continue;
                }

                thread.post_message(msg{.window = win.handle, .message = WM_PAINT, .wParam = 0, .lParam = 0, .time = 0, .pt = {}});
                win.paint_message_posted = true;
                return;
            }
        }

        void invalidate_window(const syscall_context& c, window& win, const std::optional<RECT>& update_rect = std::nullopt,
                               bool erase = false)
        {
            win.update_pending = true;
            win.erase_pending = win.erase_pending || erase;
            win.update_rect = update_rect.value_or(get_client_rect(win));

            if (win.host_surface_window)
            {
                c.win_emu.ui().invalidate(win.handle, update_rect);
            }

            queue_window_paint(c, win);
        }

        void update_window_title(const syscall_context& c, window& win, const std::u16string& title)
        {
            win.name = title;
            if (win.host_surface_window)
            {
                c.win_emu.ui().set_window_title(win.handle, win.name);
            }
        }

        std::u16string read_guest_window_text(const syscall_context& c, const window& win)
        {
            const auto guest_win = win.guest.read();
            if (guest_win.strText == 0 || guest_win.dwTextLengthBytes == 0)
            {
                return win.name;
            }

            auto text = read_string<char16_t>(c.win_emu.memory, guest_win.strText, guest_win.dwTextLengthBytes / sizeof(char16_t));
            while (!text.empty() && text.back() == u'\0')
            {
                text.pop_back();
            }
            return text;
        }

        std::u16string normalize_dialog_button_caption(std::u16string text)
        {
            text.erase(std::ranges::remove(text, u'&').begin(), text.end());
            return text;
        }

        uint64_t map_dialog_button_caption_to_id(const std::u16string_view caption)
        {
            if (caption == u"OK")
            {
                return IDOK;
            }
            if (caption == u"Cancel")
            {
                return IDCANCEL;
            }
            if (caption == u"Abort")
            {
                return IDABORT;
            }
            if (caption == u"Retry")
            {
                return IDRETRY;
            }
            if (caption == u"Ignore")
            {
                return IDIGNORE;
            }
            if (caption == u"Yes")
            {
                return IDYES;
            }
            if (caption == u"No")
            {
                return IDNO;
            }
            return 0;
        }

        std::vector<std::u16string> read_msgbox_button_titles(const syscall_context& c, const window& parent)
        {
            std::vector<std::u16string> titles{};
            const auto guest_parent = parent.guest.read();
            if (guest_parent.userData == 0)
            {
                return titles;
            }

            uint64_t button_array = 0;
            uint32_t button_count = 0;
            c.win_emu.memory.try_read_memory(guest_parent.userData + 0x68, &button_array, sizeof(button_array));
            c.win_emu.memory.try_read_memory(guest_parent.userData + 0x70, &button_count, sizeof(button_count));
            if (button_array == 0 || button_count == 0 || button_count > 16)
            {
                return titles;
            }

            titles.reserve(button_count);
            for (uint32_t i = 0; i < button_count; ++i)
            {
                uint64_t str_ptr = 0;
                c.win_emu.memory.try_read_memory(button_array + static_cast<uint64_t>(i) * sizeof(uint64_t), &str_ptr, sizeof(str_ptr));
                if (str_ptr == 0)
                {
                    titles.emplace_back();
                    continue;
                }

                std::u16string text{};
                for (size_t n = 0; n < 64; ++n)
                {
                    char16_t ch = 0;
                    c.win_emu.memory.try_read_memory(str_ptr + n * sizeof(char16_t), &ch, sizeof(ch));
                    if (ch == 0)
                    {
                        break;
                    }
                    text.push_back(ch);
                }
                titles.push_back(normalize_dialog_button_caption(std::move(text)));
            }

            return titles;
        }

        void sync_child_control_titles_from_guest(const syscall_context& c, const window& parent)
        {
            std::vector<window*> empty_buttons{};

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle != parent.handle)
                {
                    continue;
                }

                const auto normalized = normalize_builtin_window_class_name(child.class_name);
                if (normalized == u"Button")
                {
                    const auto text = read_guest_window_text(c, child);
                    if (text.empty() && child.name.empty())
                    {
                        empty_buttons.push_back(&child);
                    }
                    else if (text != child.name)
                    {
                        update_window_title(c, child, text);
                    }
                    continue;
                }

                if (normalized == u"Static")
                {
                    const auto text = read_guest_window_text(c, child);
                    if (text != child.name)
                    {
                        update_window_title(c, child, text);
                    }
                }
            }

            if (!empty_buttons.empty())
            {
                auto titles = read_msgbox_button_titles(c, parent);
                if (titles.size() == empty_buttons.size())
                {
                    std::ranges::sort(empty_buttons, [](const window* a, const window* b) { return a->x < b->x; });
                    for (size_t i = 0; i < empty_buttons.size(); ++i)
                    {
                        if (!titles[i].empty())
                        {
                            update_window_title(c, *empty_buttons[i], titles[i]);
                            if (const auto id = map_dialog_button_caption_to_id(titles[i]); id != 0)
                            {
                                empty_buttons[i]->guest.access([&](USER_WINDOW& guest_win) { guest_win.wID = id; });
                            }
                        }
                    }
                }
            }
        }

        void validate_window(window& win)
        {
            win.update_pending = false;
            win.paint_message_posted = false;
            win.erase_pending = false;
            win.update_rect = {};
        }

        template <typename T>
        void dispatch_window_message(const syscall_context& c, callback_id id, T&& state, const window& win, uint32_t message,
                                     uint64_t w_param = 0, uint64_t l_param = 0)
        {
            set_thread_window_context(c, win.handle, win.guest.value());

            switch (message)
            {
            case WM_CREATE:
            case WM_NCCREATE: {
                fn_in_lp_create_struct_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.lParam = l_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;
                if (l_param != 0)
                {
                    c.emu.read_memory(l_param, &args.cs, sizeof(args.cs));
                }

                dispatch_user_callback(c, id, k_fn_in_lp_create_struct_callback_id, std::forward<T>(state), args);
                return;
            }

            case WM_WINDOWPOSCHANGED: {
                fn_in_lp_window_pos_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;
                if (l_param != 0)
                {
                    c.emu.read_memory(l_param, &args.wp, sizeof(args.wp));
                }

                dispatch_user_callback(c, id, k_fn_in_lp_window_pos_callback_id, std::forward<T>(state), args);
                return;
            }

            case WM_WINDOWPOSCHANGING:
            case WM_GETMINMAXINFO: {
                fn_inout_lp_point5_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;
                if (l_param != 0)
                {
                    if (message == WM_WINDOWPOSCHANGING)
                    {
                        c.emu.read_memory(l_param, &args.data.window_pos, sizeof(args.data.window_pos));
                    }
                    else
                    {
                        c.emu.read_memory(l_param, &args.data.point5, sizeof(args.data.point5));
                    }
                }

                dispatch_user_callback(c, id, k_fn_inout_lp_point5_callback_id, std::forward<T>(state), args);
                return;
            }

            case WM_NCCALCSIZE: {
                fn_inout_nc_calc_size_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;
                if (l_param != 0)
                {
                    if (w_param == FALSE)
                    {
                        c.emu.read_memory(l_param, &args.u.rect, sizeof(args.u.rect));
                    }
                    else
                    {
                        c.emu.read_memory(l_param, &args.u.data.params, sizeof(args.u.data.params));
                        if (args.u.data.params.lppos != 0)
                        {
                            c.emu.read_memory(args.u.data.params.lppos, &args.u.data.pos, sizeof(args.u.data.pos));
                        }
                    }
                }

                dispatch_user_callback(c, id, k_fn_inout_nc_calc_size_callback_id, std::forward<T>(state), args);
                return;
            }

            case WM_NCDESTROY: {
                fn_dword_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.lParam = l_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;

                dispatch_user_callback(c, id, k_fn_nc_destroy_callback_id, std::forward<T>(state), args);
                return;
            }

            default: {
                fn_dword_message args{};
                args.pwnd = win.guest.value();
                args.msg = message;
                args.wParam = w_param;
                args.lParam = l_param;
                args.xParam = win.wnd_proc;
                args.xpfnProc = c.proc.dispatch_client_message;

                dispatch_user_callback(c, id, k_fn_dword_callback_id, std::forward<T>(state), args);
                return;
            }
            }
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

        std::u16string read_unicode_string_or_atom(const syscall_context& c,
                                                   const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> uc_string,
                                                   const size_t index = 0)
        {
            if (!uc_string)
            {
                return {};
            }

            if (uc_string.value() <= std::numeric_limits<uint16_t>::max())
            {
                return c.proc.get_atom_name(static_cast<uint16_t>(uc_string.value())).value_or(u"");
            }

            return read_unicode_string(c.emu, uc_string, index);
        }

        std::u16string read_large_string_or_atom(const syscall_context& c, const emulator_object<LARGE_STRING> str_obj,
                                                 const size_t index = 0)
        {
            if (!str_obj)
            {
                return {};
            }

            if (str_obj.value() <= std::numeric_limits<uint16_t>::max())
            {
                return c.proc.get_atom_name(static_cast<uint16_t>(str_obj.value())).value_or(u"");
            }

            return read_large_string(str_obj, index);
        }

        std::u16string standard_dialog_button_caption(const uint64_t id)
        {
            switch (id)
            {
            case IDOK:
                return u"OK";
            case IDCANCEL:
                return u"Cancel";
            case IDABORT:
                return u"Abort";
            case IDRETRY:
                return u"Retry";
            case IDIGNORE:
                return u"Ignore";
            case IDYES:
                return u"Yes";
            case IDNO:
                return u"No";
            default:
                return {};
            }
        }

        std::u16string decode_dialog_control_title(const syscall_context& c, const std::u16string_view normalized_class,
                                                   const emulator_object<LARGE_STRING> window_name)
        {
            if (!window_name)
            {
                return {};
            }

            const auto raw = window_name.read();
            if (raw.bAnsi)
            {
                return read_large_string(window_name);
            }

            if (raw.Buffer == 0 || raw.Length == 0)
            {
                return {};
            }

            uint16_t w0 = 0;
            c.win_emu.memory.try_read_memory(raw.Buffer, &w0, sizeof(w0));

            if (normalized_class == u"Static" && w0 == 0xFFFF)
            {
                return {};
            }

            if (normalized_class == u"Button" && raw.Length == sizeof(uint16_t))
            {
                if (const auto caption = standard_dialog_button_caption(w0); !caption.empty())
                {
                    return caption;
                }
            }

            return read_large_string(window_name);
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

        uint64_t handle_NtUserGetThreadState(const syscall_context& c, const ULONG routine)
        {
            if (routine != k_thread_state_win32_thread_info)
            {
                return 0;
            }

            const auto thread_info = ensure_win32_thread_info(c);
            if (thread_info == 0)
            {
                return 0;
            }

            publish_win32_thread_info(c, thread_info);

            if (c.proc.is_wow64_process && c.proc.active_thread && !c.proc.active_thread->win32k_thread_setup_done &&
                !c.proc.active_thread->win32k_thread_setup_pending)
            {
                c.proc.active_thread->win32k_thread_setup_pending = true;
                dispatch_user_callback(c, callback_id::NtUserGetThreadState, k_client_setup_callback_id);
                return 0;
            }

            return thread_info;
        }

        uint64_t completion_NtUserGetThreadState(const syscall_context& c, const ULONG routine)
        {
            return handle_NtUserGetThreadState(c, routine);
        }

        NTSTATUS handle_NtUserProcessConnect(const syscall_context& c, const handle process_handle, const ULONG length,
                                             const emulator_pointer user_connect)
        {
            if (!c.proc.is_wow64_process)
            {
                return STATUS_NOT_SUPPORTED;
            }

            if (!c.proc.is_current_process_handle(process_handle))
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

        hdc handle_NtUserBeginPaint(const syscall_context& c, const hwnd window, const emulator_object<EMU_PAINTSTRUCT> paint_struct)
        {
            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return 0;
            }

            const auto dc = handle_NtUserGetDCEx(c, window, 0, 0);
            if (!dc)
            {
                return 0;
            }

            if (paint_struct)
            {
                EMU_PAINTSTRUCT ps{};
                ps.paint_hdc = dc;
                ps.fErase = win->erase_pending ? TRUE : FALSE;
                ps.rcPaint = win->update_pending ? win->update_rect : get_client_rect(*win);
                ps.fRestore = FALSE;
                ps.fIncUpdate = FALSE;
                paint_struct.write(ps);
            }

            return dc;
        }

        BOOL handle_NtUserEndPaint(const syscall_context& c, const hwnd window, const emulator_object<EMU_PAINTSTRUCT> /*paint_struct*/)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            validate_window(*win);
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

        uint64_t handle_NtUserGetCursor()
        {
            return 0;
        }

        NTSTATUS handle_NtUserFindExistingCursorIcon()
        {
            return STATUS_NOT_SUPPORTED;
        }

        BOOL handle_NtUserMessageBeep()
        {
            return TRUE;
        }

        uint64_t handle_NtUserFindWindowEx(const syscall_context& c, const hwnd parent, const hwnd child_after,
                                           const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                           const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> window_name)
        {
            const auto want_class = class_name ? read_unicode_string_or_atom(c, class_name) : std::u16string{};
            const auto want_name = window_name ? read_unicode_string(c.emu, window_name) : std::u16string{};
            const bool filter_class = class_name && !want_class.empty();
            const bool filter_name = window_name && !want_name.empty();

            bool seen_child_after = child_after == 0;
            for (const auto& [index, candidate] : c.proc.windows)
            {
                (void)index;
                if (parent != 0)
                {
                    if (candidate.parent_handle != parent)
                    {
                        continue;
                    }
                }
                else if (candidate.parent_handle != c.proc.default_desktop_window_handle.bits)
                {
                    continue;
                }

                if (!seen_child_after)
                {
                    if (candidate.handle == child_after)
                    {
                        seen_child_after = true;
                    }
                    continue;
                }

                if (filter_class && candidate.class_name != want_class)
                {
                    continue;
                }
                if (filter_name && candidate.name != want_name)
                {
                    continue;
                }

                return candidate.handle;
            }

            return 0;
        }

        BOOL handle_NtUserMoveWindow(const syscall_context& c, const hwnd hwnd, const int x, const int y, const int width, const int height,
                                     const BOOL repaint)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            update_window_geometry(c, *win, x, y, width, height, repaint != FALSE);
            return TRUE;
        }

        uint64_t handle_NtUserGetProcessWindowStation()
        {
            return 0;
        }

        uint64_t handle_NtUserCallHwndParam(const syscall_context& c, const hwnd hwnd, const uint64_t param, const uint32_t code)
        {
            (void)hwnd;
            (void)param;
            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("NtUserCallHwndParam code=" + std::to_string(code));
            }

            return 0;
        }

        uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, const emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*class_version*/,
                                                 const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name,
                                                 const DWORD /*function_id*/, const DWORD /*flags*/, const emulator_pointer /*wow*/)
        {
            if (!wnd_class_ex)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            const auto class_name_str = read_unicode_string(c.emu, class_name);
            const auto index = c.proc.add_or_find_atom(class_name_str);

            constexpr auto cls_size = static_cast<size_t>(page_align_up(sizeof(USER_CLASS)));
            const auto cls_ptr = c.win_emu.memory.allocate_memory(cls_size, memory_permission::read);

            const auto wnd_class = wnd_class_ex.read();
            const auto entry = process_context::class_entry{cls_ptr, wnd_class, class_menu_name.read()};

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("RegisterClassEx name='" + u16_to_u8(class_name_str) + "' atom=#" +
                                                        std::to_string(index));
            }

            c.proc.classes.insert_or_assign(class_name_str, entry);
            c.proc.classes.insert_or_assign(make_atom_class_name(index), entry);

            return index;
        }

        BOOL handle_NtUserUnregisterClass(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                          const emulator_pointer /*instance*/,
                                          const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name)
        {
            const auto cls_name = read_unicode_string_or_atom(c, class_name);

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
            std::u16string name_str = read_unicode_string_or_atom(c, class_name);

            auto it = c.proc.classes.find(name_str);
            if (it == c.proc.classes.end())
            {
                if (const auto* builtin = ensure_builtin_window_class(c, name_str))
                {
                    it = c.proc.classes.find(name_str);
                    (void)builtin;
                }
            }

            if (it == c.proc.classes.end())
            {
                if (c.win_emu.callbacks.on_generic_activity)
                {
                    c.win_emu.callbacks.on_generic_activity("GetClassInfoEx missing class '" + u16_to_u8(name_str) + "'");
                }

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

        hwnd handle_NtUserCreateWindowEx(const syscall_context& c, const DWORD ex_style, const emulator_object<LARGE_STRING> class_name,
                                         const emulator_object<LARGE_STRING> /*cls_version*/,
                                         const emulator_object<LARGE_STRING> window_name, const DWORD style, const int x, const int y,
                                         const int width, const int height, const hwnd parent, const hmenu menu, const hinstance instance,
                                         const pointer l_param, const DWORD /*flags*/, const pointer /*acbi_buffer*/)
        {
            const auto cls_name = read_large_string_or_atom(c, class_name);
            if (c.win_emu.callbacks.on_generic_activity)
            {
                auto style_string = utils::string::to_hex_number(style);
                style_string.insert(style_string.begin(), std::max(0, 8 - static_cast<int>(style_string.size())), '0');
                c.win_emu.callbacks.on_generic_activity("CreateWindowEx class='" + u16_to_u8(cls_name) + "' style=0x" + style_string);
            }

            auto cls_it = c.proc.classes.find(cls_name);

            if (cls_it == c.proc.classes.end())
            {
                if (const auto* builtin = ensure_builtin_window_class(c, cls_name))
                {
                    cls_it = c.proc.classes.find(cls_name);
                    (void)builtin;
                }
            }

            if (cls_it == c.proc.classes.end())
            {
                if (c.win_emu.callbacks.on_generic_activity)
                {
                    c.win_emu.callbacks.on_generic_activity("CreateWindowEx missing class '" + u16_to_u8(cls_name) + "'");
                }

                set_guest_last_error(c, 1407); // ERROR_CANNOT_FIND_WND_CLASS
                return 0;
            }

            const bool is_message_only = parent == reinterpret_cast<pointer>(HWND_MESSAGE);
            const bool has_child_parent = (style & WS_CHILD) != 0 && (style & WS_POPUP) == 0;
            const bool has_owner = parent != 0 && !is_message_only && !has_child_parent;

            if (has_child_parent && !parent)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            window* parent_win = nullptr;
            if (parent && !is_message_only)
            {
                parent_win = c.proc.windows.get(parent);
                if (!parent_win)
                {
                    for (auto& [index, candidate] : c.proc.windows)
                    {
                        (void)index;
                        if (candidate.guest.value() == parent)
                        {
                            parent_win = &candidate;
                            break;
                        }
                    }
                }
                if (!parent_win)
                {
                    set_guest_last_error(c, 6); // ERROR_INVALID_HANDLE
                    return 0;
                }
            }

            const auto class_obj_addr = cls_it->second.guest_obj_addr;
            const auto* wnd_class = &cls_it->second.wnd_class;
            const auto skip_create_messages = cls_name == u"Button" || cls_name == u"Static";

            auto [handle, win] = c.proc.windows.create(c.win_emu.memory);
            win.ex_style = ex_style;
            win.style = style;
            win.x = x;
            win.y = y;
            win.width = width;
            win.height = height;
            win.thread_id = c.win_emu.current_thread().id;
            win.handle = handle.bits;
            if (!is_message_only)
            {
                win.parent_handle = has_child_parent ? parent : c.proc.default_desktop_window_handle.bits;
                win.owner_handle = has_owner ? parent : 0;
            }
            win.class_name = cls_name;
            const auto normalized_class = normalize_builtin_window_class_name(cls_name);
            if (normalized_class == u"Button" || normalized_class == u"Static")
            {
                win.name = decode_dialog_control_title(c, normalized_class, window_name);
                if (normalized_class == u"Button")
                {
                    const auto raw = window_name ? window_name.read() : LARGE_STRING{};
                    uint16_t w0 = 0;
                    if (raw.Buffer != 0 && raw.Length >= 2)
                    {
                        c.win_emu.memory.try_read_memory(raw.Buffer, &w0, sizeof(w0));
                    }
                }
            }
            else
            {
                win.name = read_large_string(window_name);
            }
            win.wnd_proc = wnd_class->lpfnWndProc;

            win.guest.access([&](USER_WINDOW& guest_win) {
                guest_win.hWnd = handle.bits;
                guest_win.ptrBase = win.guest.value();
                guest_win.dwExStyle = ex_style;
                guest_win.dwStyle = style;
                guest_win.rcUnknown_048 = {.left = x, .top = y, .right = x + width, .bottom = y + height};
                guest_win.rcWindow = guest_win.rcUnknown_048;
                guest_win.rcClient = guest_win.rcWindow;
                if (parent_win && has_child_parent)
                {
                    guest_win.spwndParent = parent_win->guest.value();
                }
                else if (is_message_only)
                {
                    guest_win.spwndParent = 0;
                }
                else if (const auto* wnd = c.proc.windows.get(c.proc.default_desktop_window_handle))
                {
                    guest_win.spwndParent = wnd->guest.value();
                }
                guest_win.spwndOwner = parent_win && has_owner ? parent_win->guest.value() : 0;
                guest_win.lpfnWndProc = win.wnd_proc;
                guest_win.pcls = class_obj_addr;
                guest_win.cbWndExtra = wnd_class->cbWndExtra;
                guest_win.wID = has_child_parent ? menu : 0;
                guest_win.windowBand = 1; // ZBID_DESKTOP

                win.host_surface_window = !is_message_only;

                if (wnd_class->cbWndExtra > 0)
                {
                    const auto extra_size = static_cast<size_t>(page_align_up(wnd_class->cbWndExtra));
                    guest_win.pExtraBytes = c.win_emu.memory.allocate_memory(extra_size, memory_permission::read_write);
                }
            });

            if (has_child_parent && parent_win && parent_win->guest.value() != 0)
            {
                constexpr uint64_t k_spwnd_child_offset = 0x38;
                constexpr uint64_t k_spwnd_next_offset = 0x48;
                constexpr uint64_t k_null_ptr = 0;

                c.win_emu.memory.write_memory(win.guest.value() + k_spwnd_next_offset, &k_null_ptr, sizeof(k_null_ptr));

                uint64_t first_child = 0;
                c.win_emu.memory.try_read_memory(parent_win->guest.value() + k_spwnd_child_offset, &first_child, sizeof(first_child));
                if (first_child == 0)
                {
                    const auto child_ptr = win.guest.value();
                    c.win_emu.memory.write_memory(parent_win->guest.value() + k_spwnd_child_offset, &child_ptr, sizeof(child_ptr));
                }
                else
                {
                    uint64_t current = first_child;
                    uint64_t next = 0;
                    while (current != 0)
                    {
                        c.win_emu.memory.try_read_memory(current + k_spwnd_next_offset, &next, sizeof(next));
                        if (next == 0)
                        {
                            const auto child_ptr = win.guest.value();
                            c.win_emu.memory.write_memory(current + k_spwnd_next_offset, &child_ptr, sizeof(child_ptr));
                            break;
                        }
                        current = next;
                    }
                }
            }

            if (!is_message_only)
            {
                c.win_emu.ui().create_window(ui_window_desc{
                    .handle = handle.bits,
                    .parent = has_child_parent && parent_win ? parent_win->handle : 0,
                    .owner = has_owner && parent_win ? parent_win->handle : 0,
                    .rect = get_window_rect(win),
                    .class_name = std::u16string{normalize_builtin_window_class_name(cls_name)},
                    .title = win.name,
                    .style = style,
                    .ex_style = ex_style,
                    .control_id = has_child_parent ? static_cast<uint32_t>(menu) : 0,
                    .visible = (style & WS_VISIBLE) != 0,
                    .enabled = (style & WS_DISABLED) == 0,
                    .top_level = !has_child_parent,
                });
            }

            const auto thread_info = ensure_win32_thread_info(c);
            publish_win32_thread_info(c, thread_info);
            set_user_handle_owner(c, handle, thread_info);

            if (skip_create_messages)
            {
                if ((style & WS_VISIBLE) != 0)
                {
                    invalidate_window(c, win);
                }

                if (c.win_emu.callbacks.on_generic_activity)
                {
                    c.win_emu.callbacks.on_generic_activity("CreateWindowEx builtin success class='" + u16_to_u8(cls_name) + "' hwnd=0x" +
                                                            utils::string::to_hex_number(handle.bits));
                }

                return handle.bits;
            }

            window_create_state state{};
            state.handle = handle.bits;

            EMU_CREATESTRUCT cs{};
            cs.lpCreateParams = l_param;
            cs.hInstance = instance;
            cs.hMenu = menu;
            cs.hwndParent = parent;
            cs.cy = height;
            cs.cx = width;
            cs.y = y;
            cs.x = x;
            cs.style = static_cast<LONG>(style);
            cs.lpszName =
                window_name && window_name.value() > std::numeric_limits<uint16_t>::max() ? window_name.read().Buffer : window_name.value();
            cs.lpszClass =
                class_name && class_name.value() > std::numeric_limits<uint16_t>::max() ? class_name.read().Buffer : class_name.value();
            cs.dwExStyle = ex_style;
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
                invalidate_window(c, win);

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

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("CreateWindowEx async class='" + u16_to_u8(cls_name) + "' hwnd=0x" +
                                                        utils::string::to_hex_number(handle.bits));
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

            c.win_emu.ui().destroy_window(window);
            return c.proc.windows.erase(window);
        }

        BOOL handle_NtUserSetProp(const syscall_context& c, const hwnd window, const uint16_t atom, const uint64_t data)
        {
            auto* win = c.proc.windows.get(window);
            const auto prop = c.proc.get_atom_name(atom);

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

            auto prop = read_unicode_string_or_atom(c, str);
            if (prop.empty())
            {
                return FALSE;
            }

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

            if (win->host_surface_window)
            {
                if (want_visible && normalize_builtin_window_class_name(win->class_name) == u"#32770")
                {
                    sync_child_control_titles_from_guest(c, *win);
                }
                c.win_emu.ui().set_window_visible(hwnd, want_visible);
            }

            if (want_visible)
            {
                invalidate_window(c, *win);

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

        bool handle_dialog_message(const syscall_context& c, window& dialog, uint32_t message, uint64_t w_param, uint64_t l_param);

        uint64_t handle_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t w_param,
                                          const uint64_t l_param, const uint64_t /*result_info*/, const DWORD /*type*/, const BOOL ansi)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            if (win->thread_id != c.proc.active_thread->id)
            {
                return 0;
            }

            if (msg == WM_SETTEXT)
            {
                if (l_param == 0)
                {
                    update_window_title(c, *win, {});
                }
                else if (ansi)
                {
                    const auto text = u8_to_u16(read_string<char>(c.win_emu.memory, l_param));
                    update_window_title(c, *win, text);
                }
                else
                {
                    const auto text = read_string<char16_t>(c.win_emu.memory, l_param);
                    update_window_title(c, *win, text);
                }
            }

            (void)handle_dialog_message(c, *win, msg, w_param, l_param);

            dispatch_window_message(c, callback_id::NtUserMessageCall, nullptr, *win, msg, w_param, l_param);
            return {};
        }

        uint64_t completion_NtUserMessageCall(const syscall_context& c, const hwnd /*hwnd*/, const UINT /*msg*/, const uint64_t /*w_param*/,
                                              const uint64_t /*l_param*/, const uint64_t /*result_info*/, const DWORD /*type*/,
                                              const BOOL /*ansi*/)
        {
            return c.get_callback_result<uint64_t>();
        }

        uint64_t handle_NtUserDispatchMessage(const syscall_context& c, const emulator_object<msg> message)
        {
            if (!message)
            {
                return 0;
            }

            const auto m = message.read();
            auto* win = c.proc.windows.get(m.window);
            if (!win)
            {
                return 0;
            }

            if (win->thread_id != c.proc.active_thread->id)
            {
                return 0;
            }

            (void)handle_dialog_message(c, *win, m.message, m.wParam, m.lParam);

            dispatch_window_message(c, callback_id::NtUserMessageCall, nullptr, *win, m.message, m.wParam, m.lParam);
            return {};
        }

        BOOL handle_NtUserTranslateMessage(const syscall_context& /*c*/, const emulator_object<msg> /*message*/, const UINT /*flags*/)
        {
            return FALSE;
        }

        BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                     const UINT msg_filter_min, const UINT msg_filter_max)
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

        void complete_dialog(const syscall_context& c, window& dialog, const uint64_t result)
        {
            if (dialog.dialog_pointer == 0)
            {
                return;
            }

            constexpr uint32_t dialog_flag_end = 0x1;
            uint32_t flags{};
            c.win_emu.memory.read_memory(dialog.dialog_pointer + 0x18, &flags, sizeof(flags));
            flags |= dialog_flag_end;
            c.win_emu.memory.write_memory(dialog.dialog_pointer + 0x18, &flags, sizeof(flags));
            c.win_emu.memory.write_memory(dialog.dialog_pointer + 0x20, &result, sizeof(result));
            dialog.dialog_flags = flags;
            dialog.dialog_result = result;
        }

        bool is_dialog_window(const window& win)
        {
            return normalize_builtin_window_class_name(win.class_name) == u"#32770";
        }

        bool handle_dialog_message(const syscall_context& c, window& dialog, const uint32_t message, const uint64_t w_param,
                                   const uint64_t l_param)
        {
            if (!is_dialog_window(dialog))
            {
                return false;
            }

            switch (message)
            {
            case WM_COMMAND: {
                const auto control_id = static_cast<uint32_t>(w_param & 0xFFFF);
                const auto notification = static_cast<uint32_t>((w_param >> 16) & 0xFFFF);
                if (notification == BN_CLICKED && l_param != 0)
                {
                    complete_dialog(c, dialog, control_id != 0 ? control_id : IDOK);
                    return true;
                }
                break;
            }

            case WM_KEYDOWN:
                if (w_param == VK_RETURN)
                {
                    complete_dialog(c, dialog, IDOK);
                    return true;
                }
                if (w_param == VK_ESCAPE)
                {
                    complete_dialog(c, dialog, IDCANCEL);
                    return true;
                }
                break;

            case WM_CLOSE:
                complete_dialog(c, dialog, IDCANCEL);
                return true;

            default:
                break;
            }

            return false;
        }

        BOOL handle_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                      const UINT msg_filter_min, const UINT msg_filter_max, const UINT remove_message)
        {
            auto& t = c.win_emu.current_thread();

            const bool should_remove = (remove_message & PM_REMOVE) != 0;
            std::optional<msg> pending_msg = t.peek_pending_message(hwnd, msg_filter_min, msg_filter_max, should_remove);

            if (pending_msg)
            {
                if (should_remove)
                {
                    if (auto* win = c.proc.windows.get(pending_msg->window))
                    {
                        (void)handle_dialog_message(c, *win, pending_msg->message, pending_msg->wParam, pending_msg->lParam);
                    }
                }
                message.write(*pending_msg);
                set_thread_window_context(c, pending_msg->window);
                return TRUE;
            }

            return FALSE;
        }

        BOOL handle_NtUserWaitMessage(const syscall_context& c)
        {
            auto& t = c.win_emu.current_thread();
            if (t.peek_pending_message(0, 0, 0, false))
            {
                return TRUE;
            }

            c.win_emu.yield_thread(false);
            return {};
        }

        BOOL handle_NtUserInvalidateRect(const syscall_context& c, const hwnd hwnd, const emulator_object<RECT> rect, const BOOL erase)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            std::optional<RECT> update_rect{};
            if (rect)
            {
                update_rect = rect.read();
            }

            invalidate_window(c, *win, update_rect, erase != FALSE);
            return TRUE;
        }

        BOOL handle_NtUserValidateRect(const syscall_context& c, const hwnd hwnd, const emulator_object<RECT> /*rect*/)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            validate_window(*win);
            return TRUE;
        }

        BOOL handle_NtUserUpdateWindow(const syscall_context& c, const hwnd hwnd)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            if (win->update_pending)
            {
                queue_window_paint(c, *win);
            }
            return TRUE;
        }

        BOOL handle_NtUserPostMessage(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t wParam,
                                      const uint64_t lParam)
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
                    sogen::msg qmsg{};
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
            sogen::msg qmsg{};
            qmsg.message = WM_QUIT;
            qmsg.wParam = exit_code;

            c.proc.active_thread->post_message(qmsg);
            return TRUE;
        }

        NTSTATUS handle_NtUserEnumDisplayDevices(const syscall_context& c,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str_device,
                                                 const DWORD dev_num, const emulator_object<EMU_DISPLAY_DEVICEW> display_device,
                                                 const DWORD /*flags*/)
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
                const auto dev_name = device_name ? read_unicode_string(c.emu, device_name) : u"";

                if (dev_name.empty() || dev_name == u"\\\\.\\DISPLAY1")
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

        BOOL handle_NtUserEnumDisplayMonitors(const syscall_context& c, const hdc hdc_in, const uint64_t clip_rect_ptr,
                                              const uint64_t callback, const uint64_t param)
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

            const enum_display_monitors_callback_args args{
                .monitor = hmon,
                .dc = hdc_in,
                .rect = effective_rc,
                .data = param,
                .callback = callback,
            };

            dispatch_user_callback(c, callback_id::NtUserEnumDisplayMonitors, k_enum_display_monitors_callback_id, args);
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

        BOOL handle_NtUserTransformRect(const syscall_context& c, const emulator_object<RECT> rect, const hwnd hwnd,
                                        const uint32_t /*type*/, const uint64_t /*unknown*/)
        {
            if (!rect)
            {
                return FALSE;
            }

            const auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            rect.write(get_window_rect(*win));
            return TRUE;
        }

        BOOL handle_NtUserSetWindowPos(const syscall_context& c, const hwnd hWnd, const hwnd /*hwnd_insert_after*/, const int x,
                                       const int y, const int cx, const int cy, const UINT flags)
        {
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                return FALSE;
            }

            const auto new_x = (flags & SWP_NOMOVE) ? win->x : x;
            const auto new_y = (flags & SWP_NOMOVE) ? win->y : y;
            const auto new_width = (flags & SWP_NOSIZE) ? win->width : cx;
            const auto new_height = (flags & SWP_NOSIZE) ? win->height : cy;
            const auto repaint = (flags & SWP_NOREDRAW) == 0;

            update_window_geometry(c, *win, new_x, new_y, new_width, new_height, repaint);

            if ((flags & SWP_HIDEWINDOW) != 0)
            {
                win->style &= ~WS_VISIBLE;
                win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win->style; });
                if (win->host_surface_window)
                {
                    c.win_emu.ui().set_window_visible(hWnd, false);
                }
            }
            else if ((flags & SWP_SHOWWINDOW) != 0)
            {
                win->style |= WS_VISIBLE;
                win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win->style; });
                if (win->host_surface_window)
                {
                    c.win_emu.ui().set_window_visible(hWnd, true);
                }
            }

            return TRUE;
        }

        NTSTATUS handle_NtUserSetForegroundWindow()
        {
            return STATUS_SUCCESS;
        }

        hwnd handle_NtUserGetForegroundWindow()
        {
            return 0;
        }

        hwnd handle_NtUserSetFocus(const syscall_context& c, const hwnd hwnd)
        {
            if (hwnd != 0)
            {
                set_thread_window_context(c, hwnd);
            }
            return hwnd;
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

                    case GWLP_ID:
                        oldValue = guest_win.wID;
                        guest_win.wID = dwNewLong;
                        if (normalize_builtin_window_class_name(win->class_name) == u"Button")
                        {
                            if (win->name.empty())
                            {
                                if (const auto caption = standard_dialog_button_caption(dwNewLong); !caption.empty())
                                {
                                    update_window_title(c, *win, caption);
                                }
                            }
                        }
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

        uint64_t handle_NtUserGetAncestor(const syscall_context& c, const hwnd child_hwnd, const UINT flags)
        {
            const auto* win = c.proc.windows.get(child_hwnd);
            if (!win)
            {
                return 0;
            }

            const hwnd desktop = c.proc.default_desktop_window_handle.bits;

            const auto get_root = [&](const hwnd start) -> hwnd {
                if (!start || start == desktop)
                {
                    return 0;
                }

                hwnd current = start;

                for (;;)
                {
                    const auto* current_win = c.proc.windows.get(current);
                    if (!current_win)
                    {
                        return 0;
                    }

                    const hwnd parent = current_win->parent_handle;
                    if (!parent || parent == desktop)
                    {
                        return current;
                    }

                    current = parent;
                }
            };

            switch (flags)
            {
            case 1: // GA_PARENT
                if (child_hwnd == desktop)
                {
                    return 0;
                }

                return win->parent_handle;

            case 2: // GA_ROOT
                return get_root(child_hwnd);

            case 3: { // GA_ROOTOWNER
                hwnd current = get_root(child_hwnd);

                while (current)
                {
                    const auto* current_win = c.proc.windows.get(current);
                    if (!current_win || !current_win->owner_handle)
                    {
                        return current;
                    }

                    if ((current_win->style & WS_POPUP) == 0)
                    {
                        return current;
                    }

                    const hwnd owner_root = get_root(current_win->owner_handle);
                    if (!owner_root || owner_root == current)
                    {
                        return current;
                    }

                    current = owner_root;
                }

                return 0;
            }

            default:
                return 0;
            }
        }

        BOOL handle_NtUserRedrawWindow(const syscall_context& c, const hwnd hwnd, const emulator_object<RECT> update_rect,
                                       const uint64_t /*update_rgn*/, const UINT flags)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            if ((flags & RDW_VALIDATE) != 0)
            {
                validate_window(*win);
            }

            if ((flags & (RDW_INVALIDATE | RDW_INTERNALPAINT)) != 0)
            {
                std::optional<RECT> rect{};
                if (update_rect)
                {
                    rect = update_rect.read();
                }

                invalidate_window(c, *win, rect, (flags & RDW_ERASE) != 0 && (flags & RDW_NOERASE) == 0);
            }

            if ((flags & RDW_NOINTERNALPAINT) != 0)
            {
                win->paint_message_posted = false;
            }

            return TRUE;
        }

        NTSTATUS handle_NtUserGetCPD()
        {
            return STATUS_SUCCESS;
        }

        BOOL handle_NtUserSetWindowFNID(const syscall_context& c, const hwnd hwnd, const WORD fnid)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            win->guest.access([&](USER_WINDOW& guest_win) { guest_win.fnid = fnid; });

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("SetWindowFNID hwnd=0x" + utils::string::to_hex_number(hwnd) + " fnid=0x" +
                                                        utils::string::to_hex_number(fnid));
            }

            return TRUE;
        }

        BOOL handle_NtUserSetDialogPointer(const syscall_context& c, const hwnd hwnd, const emulator_pointer ptr)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            win->dialog_pointer = ptr;
            if (ptr != 0 && (win->dialog_flags & 0x1) != 0)
            {
                c.win_emu.memory.write_memory(ptr + 0x18, &win->dialog_flags, sizeof(win->dialog_flags));
                c.win_emu.memory.write_memory(ptr + 0x20, &win->dialog_result, sizeof(win->dialog_result));
            }

            const auto guest_win = win->guest.read();
            if (guest_win.pExtraBytes != 0)
            {
                c.win_emu.memory.write_memory(guest_win.pExtraBytes + sizeof(uint64_t), &ptr, sizeof(ptr));
            }

            return TRUE;
        }

        BOOL handle_NtUserSetDialogSystemMenu(const syscall_context& c, const hwnd hwnd)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            if (win->system_menu_ptr == 0)
            {
                win->system_menu_ptr = c.win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            }

            if (win->guest.value() != 0)
            {
                const auto spmenu_addr = win->guest.value() + offsetof(USER_WINDOW, spmenu);
                c.win_emu.memory.write_memory(spmenu_addr, &win->system_menu_ptr, sizeof(win->system_menu_ptr));
            }

            return TRUE;
        }

        BOOL handle_NtUserSetMsgBox(const syscall_context& c, const hwnd hwnd)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserEnableWindow(const syscall_context& c, const hwnd hwnd, const BOOL enable)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            const auto was_enabled = (win->style & WS_DISABLED) == 0;
            const auto want_enabled = enable != FALSE;
            if (was_enabled == want_enabled)
            {
                return was_enabled ? FALSE : TRUE;
            }

            if (want_enabled)
            {
                win->style &= ~WS_DISABLED;
            }
            else
            {
                win->style |= WS_DISABLED;
            }

            win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win->style; });

            if (win->host_surface_window)
            {
                c.win_emu.ui().set_window_enabled(hwnd, want_enabled);
            }

            return was_enabled ? FALSE : TRUE;
        }

        BOOL handle_NtUserDeleteMenu(const syscall_context& /*c*/, const uint64_t /*menu*/, const UINT /*position*/, const UINT /*flags*/)
        {
            return TRUE;
        }

        uint64_t handle_NtUserGetSystemMenu(const syscall_context& c, const hwnd hwnd, const BOOL /*revert*/)
        {
            const auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            return win->system_menu_ptr;
        }

        BOOL handle_NtUserAllowSetForegroundWindow()
        {
            return TRUE;
        }

        ULONG handle_NtUserGetAtomName(const syscall_context& c, const RTL_ATOM atom,
                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> atom_name)
        {
            const auto name = c.proc.get_atom_name(atom);
            if (!name || !atom_name)
            {
                return 0;
            }

            const size_t name_length_bytes = name->size() * sizeof(char16_t);

            bool too_small = false;
            ULONG result = 0;

            atom_name.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
                if (str.MaximumLength < sizeof(char16_t) || !str.Buffer)
                {
                    str.Length = 0;
                    too_small = true;
                    return;
                }

                const auto max_copy_bytes = static_cast<size_t>(str.MaximumLength - sizeof(char16_t)) & ~size_t{1};
                const auto copy_bytes = std::min(name_length_bytes, max_copy_bytes);

                if (copy_bytes)
                {
                    c.emu.write_memory(str.Buffer, name->data(), copy_bytes);
                }

                constexpr char16_t terminator = 0;
                c.emu.write_memory(str.Buffer + copy_bytes, &terminator, sizeof(terminator));

                str.Length = static_cast<USHORT>(copy_bytes);
                result = static_cast<ULONG>(copy_bytes / sizeof(char16_t));
            });

            if (too_small)
            {
                set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                return 0;
            }

            return result;
        }
    }

} // namespace sogen
