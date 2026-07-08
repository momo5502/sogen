#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"
#include "../win32k_userconnect.hpp"
#include "../window_destroy_orchestrator.hpp"
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
        constexpr ULONG k_thread_state_dialog_state = 0xA;
        constexpr ULONG k_thread_state_message_time = 0x9;
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
        constexpr size_t k_client_pfn_button_wndproc_index = 7;
        constexpr size_t k_client_pfn_dialog_wndproc_index = 10;
        constexpr size_t k_client_pfn_static_wndproc_index = 14;
        constexpr uint32_t k_ctlcolor_edit = 1;
        constexpr uint32_t k_ctlcolor_listbox = 2;
        constexpr uint32_t k_ctlcolor_scrollbar = 5;
        constexpr uint32_t k_color_scrollbar = 0;
        constexpr uint32_t k_color_window = 5;
        constexpr uint32_t k_color_btnface = 15;
        constexpr auto k_user_timer_minimum = std::chrono::milliseconds{10};

        struct send_message_callback_info
        {
            uint64_t callback{};
            uint64_t data{};
        };

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
            c.vcpu.active_thread->teb64->access([&](TEB64& teb) {
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

        bool is_builtin_window_class_name(const std::u16string_view class_name)
        {
            const auto normalized = normalize_builtin_window_class_name(class_name);
            return normalized == builtin_dialog_class_name || normalized == u"Button" || normalized == u"Static";
        }

        uint16_t get_builtin_window_fnid(const std::u16string_view class_name)
        {
            const auto normalized = normalize_builtin_window_class_name(class_name);
            if (normalized == u"Button")
            {
                return 0x02A1;
            }
            if (normalized == builtin_dialog_class_name)
            {
                return 0x02A4;
            }
            if (normalized == u"Static")
            {
                return 0x02A8;
            }

            return 0;
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

            (void)win32k_userconnect::try_bootstrap_client_pfn_arrays_from_ntdll(c.win_emu);

            c.proc.user_handles.get_server_info().access([&](const USER_SERVERINFO& server_info) {
                if (normalized_name == u"Button")
                {
                    wnd_proc = server_info.apfnClientW[k_client_pfn_button_wndproc_index];
                    if (wnd_proc == 0)
                    {
                        wnd_proc = server_info.apfnClientA[k_client_pfn_button_wndproc_index];
                    }
                }
                else if (normalized_name == u"Static")
                {
                    wnd_proc = server_info.apfnClientW[k_client_pfn_static_wndproc_index];
                    if (wnd_proc == 0)
                    {
                        wnd_proc = server_info.apfnClientA[k_client_pfn_static_wndproc_index];
                    }
                }
                else if (normalized_name == builtin_dialog_class_name)
                {
                    wnd_proc = server_info.apfnClientW[k_client_pfn_dialog_wndproc_index];
                    if (wnd_proc == 0)
                    {
                        wnd_proc = server_info.apfnClientA[k_client_pfn_dialog_wndproc_index];
                    }
                }
            });

            if (normalized_name == u"Button" || normalized_name == u"Static")
            {
                wnd_extra = 8;
            }
            else if (normalized_name == builtin_dialog_class_name)
            {
                wnd_extra = 30; // DLGWINDOWEXTRA
            }

            if (wnd_proc == 0 && normalized_name == builtin_dialog_class_name)
            {
                wnd_proc = c.win_emu.mod_manager.ntdll->find_export("NtdllDialogWndProc_W");
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
            if (c.vcpu.active_thread && c.vcpu.active_thread->teb64)
            {
                c.vcpu.active_thread->teb64->access([&](TEB64& teb) {
                    teb.Win32ClientInfo.arr[8] = active_handle;
                    teb.Win32ClientInfo.arr[9] = active_window_ptr;
                });
            }

            if (c.proc.is_wow64_process && c.vcpu.active_thread && c.vcpu.active_thread->teb32)
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

                c.vcpu.active_thread->teb32->access([&](TEB32& teb) {
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
            return RECT{.left = 0, .top = 0, .right = win.client_width(), .bottom = win.client_height()};
        }

        RECT get_window_rect(const window& win)
        {
            return RECT{.left = win.x, .top = win.y, .right = win.x + win.width, .bottom = win.y + win.height};
        }

        ui_insets get_host_ui_client_insets(const window& win)
        {
            const auto border = win.nonclient_border();
            return {.left = border, .top = border, .right = border, .bottom = border};
        }

        void sync_guest_window_rects(window& win)
        {
            const auto window_rect = get_window_rect(win);
            // Frameless: the client sits at the window origin, so it shares the top-left and shrinks by the frame.
            const RECT client_rect{.left = win.x, .top = win.y, .right = win.x + win.client_width(), .bottom = win.y + win.client_height()};

            win.guest.access([&](USER_WINDOW& guest_win) {
                guest_win.rcWindow = window_rect;
                guest_win.rcClient = client_rect;
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

            if (auto* thread = c.proc.find_thread_by_id(win.thread_id))
            {
                thread->post_message(c.win_emu, msg{.window = win.handle, .message = WM_PAINT, .wParam = 0, .lParam = 0});
                win.paint_message_posted = true;
            }
        }

        RECT union_update_rect(const RECT& a, const RECT& b)
        {
            const auto empty_rect = [](const RECT& r) { //
                return r.left >= r.right || r.top >= r.bottom;
            };

            if (empty_rect(a))
            {
                return b;
            }

            if (empty_rect(b))
            {
                return a;
            }

            return RECT{
                .left = std::min(a.left, b.left),
                .top = std::min(a.top, b.top),
                .right = std::max(a.right, b.right),
                .bottom = std::max(a.bottom, b.bottom),
            };
        }

        void invalidate_window(const syscall_context& c, window& win, const std::optional<RECT>& update_rect = std::nullopt,
                               bool erase = false)
        {
            const RECT new_rect = update_rect.value_or(get_client_rect(win));

            win.erase_pending = win.erase_pending || erase;

            if (!win.update_pending)
            {
                win.update_rect = new_rect;
                win.update_pending = true;
            }
            else
            {
                win.update_rect = union_update_rect(win.update_rect, new_rect);
            }

            if (win.host_surface_window)
            {
                c.win_emu.ui().invalidate(win.handle, update_rect);
            }

            queue_window_paint(c, win);
        }

        // Invalidate a window together with its visible descendant controls. Used when a window
        // transitions to visible: child controls created while an ancestor was still hidden skip
        // their paint body (user32 gates control painting on the whole parent chain being visible),
        // so they must be repainted once the ancestor becomes visible.
        void invalidate_window_tree(const syscall_context& c, window& win)
        {
            invalidate_window(c, win);

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle == win.handle && (child.style & WS_VISIBLE) != 0)
                {
                    invalidate_window_tree(c, child);
                }
            }
        }

        void write_guest_window_text(const syscall_context& c, window& win, const std::u16string& title)
        {
            win.guest.access([&](USER_WINDOW& guest_win) {
                guest_win.dwTextLengthBytes = static_cast<uint32_t>(title.size() * sizeof(char16_t));
                if (title.empty())
                {
                    guest_win.strText = 0;
                    return;
                }

                const auto text_size = (title.size() + 1) * sizeof(char16_t);
                const auto text_buffer =
                    c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(text_size)), memory_permission::read_write);
                c.win_emu.memory.write_memory(text_buffer, title.c_str(), text_size);
                guest_win.strText = text_buffer;
            });
        }

        void update_window_title(const syscall_context& c, window& win, const std::u16string& title)
        {
            win.name = title;
            write_guest_window_text(c, win, title);
            if (win.host_surface_window)
            {
                c.win_emu.ui().set_window_title(win.handle, win.name);
            }

            invalidate_window(c, win);
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

        void validate_window(window& win);

        bool write_message_call_result(const syscall_context& c, const uint64_t result_info, const uint64_t result)
        {
            if (result_info == 0)
            {
                return true;
            }

            if (c.proc.is_wow64_process)
            {
                const auto result32 = static_cast<uint32_t>(result);
                return c.win_emu.memory.try_write_memory(result_info, &result32, sizeof(result32));
            }

            return c.win_emu.memory.try_write_memory(result_info, &result, sizeof(result));
        }

        uint64_t copy_def_window_text(const syscall_context& c, const window& win, const uint64_t character_count, const uint64_t buffer,
                                      const bool ansi)
        {
            if (character_count == 0 || buffer == 0)
            {
                return 0;
            }

            const auto text = read_guest_window_text(c, win);
            if (ansi)
            {
                const auto narrow = u16_to_cp1252(text);
                const auto copy_count = std::min<uint64_t>(narrow.size(), character_count - 1);
                if (copy_count != 0 && !c.win_emu.memory.try_write_memory(buffer, narrow.data(), static_cast<size_t>(copy_count)))
                {
                    return 0;
                }

                const char terminator = '\0';
                if (!c.win_emu.memory.try_write_memory(buffer + copy_count, &terminator, sizeof(terminator)))
                {
                    return 0;
                }

                return copy_count;
            }

            const auto copy_count = std::min<uint64_t>(text.size(), character_count - 1);
            if (copy_count != 0 &&
                !c.win_emu.memory.try_write_memory(buffer, text.data(), static_cast<size_t>(copy_count * sizeof(char16_t))))
            {
                return 0;
            }

            const char16_t terminator = u'\0';
            if (!c.win_emu.memory.try_write_memory(buffer + copy_count * sizeof(char16_t), &terminator, sizeof(terminator)))
            {
                return 0;
            }

            return copy_count;
        }

        uint64_t handle_default_window_proc_message(const syscall_context& c, window& win, const UINT msg, const uint64_t w_param,
                                                    const uint64_t l_param, const BOOL ansi)
        {
            switch (msg)
            {
            case WM_SETTEXT:
                if (l_param == 0)
                {
                    update_window_title(c, win, {});
                }
                else if (ansi)
                {
                    update_window_title(c, win, cp1252_to_u16(read_string<char>(c.win_emu.memory, l_param)));
                }
                else
                {
                    update_window_title(c, win, read_string<char16_t>(c.win_emu.memory, l_param));
                }
                return TRUE;

            case WM_GETTEXT:
                return copy_def_window_text(c, win, w_param, l_param, ansi != FALSE);

            case WM_GETTEXTLENGTH:
                // CP-1252 is 1:1 with UTF-16 code units, so the ANSI byte count equals the code-unit
                // count; no need to encode just to measure it.
                return read_guest_window_text(c, win).size();

            case WM_ERASEBKGND:
                return TRUE;

            case WM_PAINT:
                validate_window(win);
                return FALSE;

            default:
                return FALSE;
            }
        }

        std::u16string normalize_dialog_button_caption(std::u16string text)
        {
            text.erase(std::ranges::remove(text, u'&').begin(), text.end());
            return text;
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

        void present_existing_guest_window_surface(const syscall_context& c, const window& painted_window)
        {
            const window* top_level = &painted_window;
            while (top_level && (top_level->style & WS_CHILD) != 0)
            {
                top_level = top_level->parent_handle != 0 ? c.proc.windows.get(top_level->parent_handle) : nullptr;
            }

            if (!top_level || !top_level->host_surface_window)
            {
                return;
            }

            const auto surface_it = c.proc.gdi_window_surfaces.find(static_cast<uint32_t>(top_level->handle));
            if (surface_it == c.proc.gdi_window_surfaces.end())
            {
                return;
            }

            const auto& surface = surface_it->second;
            if (surface.width == 0 || surface.height == 0 || surface.pixels.empty())
            {
                return;
            }

            c.win_emu.ui().present_surface(top_level->handle, ui_surface_desc{.width = static_cast<int>(surface.width),
                                                                              .height = static_cast<int>(surface.height),
                                                                              .stride = static_cast<int>(surface.width * sizeof(uint32_t)),
                                                                              .format = ui_surface_format::bgra8,
                                                                              .pixels = surface.pixels.data()});
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

        BOOL advance_window_destroy(const syscall_context& c, window_destroy_state& state)
        {
            window_destroy_orchestrator orchestrator{state, c};
            const auto step = orchestrator.advance();
            if (!step)
            {
                return TRUE;
            }

            auto* win = c.proc.windows.get(step->handle);
            if (!win)
            {
                return advance_window_destroy(c, state);
            }

            dispatch_window_message(c, callback_id::NtUserDestroyWindow, std::move(state), *win, step->message.message,
                                    step->message.wParam, step->message.lParam);
            return {};
        }

        uint64_t ensure_win32_thread_info(const syscall_context& c)
        {
            auto* thread = c.vcpu.active_thread;
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
            auto* thread = c.vcpu.active_thread;
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

        // user32's dialog manager turns a control-class ordinal (0x80=Button, 0x81=Edit, 0x82=Static,
        // 0x83=ListBox, 0x84=ScrollBar, 0x85=ComboBox) into a class atom by indexing the WORD table
        // SERVERINFO.atomSysClass[ICLS] at gpsi+0x364 with (ordinal & 0x7F), then hands that atom to
        // NtUserCreateWindowEx. Those atom values are assigned per build, so the same control shows up
        // as e.g. atom 0x7F0 on one Windows version and a different value on another. Map an incoming
        // integer atom back to its canonical builtin class name by reverse-looking it up in that table,
        // which is portable across builds (no hardcoded atom values).
        std::u16string_view resolve_builtin_class_atom(const syscall_context& c, const uint16_t atom)
        {
            if (atom == 0)
            {
                return {};
            }

            static constexpr std::array<std::u16string_view, 6> class_names = {
                u"Button", u"Edit", u"Static", u"ListBox", u"ScrollBar", u"ComboBox",
            };

            constexpr uint64_t k_atom_sys_class_offset = 0x364;

            const auto serverinfo_base = c.proc.user_handles.get_server_info().value();
            if (serverinfo_base == 0)
            {
                return {};
            }

            for (size_t i = 0; i < class_names.size(); ++i)
            {
                uint16_t entry = 0;
                const auto address = serverinfo_base + k_atom_sys_class_offset + i * sizeof(uint16_t);
                if (c.win_emu.memory.try_read_memory(address, &entry, sizeof(entry)) && entry == atom)
                {
                    return class_names[i];
                }
            }

            return {};
        }

        std::u16string resolve_atom_name(const syscall_context& c, const uint16_t atom)
        {
            if (const auto builtin = resolve_builtin_class_atom(c, atom); !builtin.empty())
            {
                return std::u16string{builtin};
            }

            return c.proc.get_atom_name(atom).value_or(u"");
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
                return resolve_atom_name(c, static_cast<uint16_t>(uc_string.value()));
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
                return resolve_atom_name(c, static_cast<uint16_t>(str_obj.value()));
            }

            return read_large_string(str_obj, index);
        }

        std::u16string read_def_set_text_string(const syscall_context& c, const emulator_object<LARGE_STRING> text)
        {
            if (!text)
            {
                return {};
            }

            if (const auto str = text.try_read())
            {
                constexpr ULONG k_max_reasonable_text_bytes = 0x10000;
                const auto length = str->Length;
                const auto max_length = str->MaximumLength;
                const auto has_valid_empty_string = length == 0;
                std::byte last_byte{};
                const auto has_valid_buffer = str->Buffer != 0 && length <= k_max_reasonable_text_bytes && length <= max_length &&
                                              length > 0 &&
                                              c.win_emu.memory.try_read_memory(str->Buffer + length - 1, &last_byte, sizeof(last_byte));
                if (has_valid_empty_string)
                {
                    return {};
                }

                if (has_valid_buffer)
                {
                    // bAnsi is reliable: handle_NtUserMessageCall re-encodes WM_SETTEXT to the target
                    // proc's encoding, so the buffer agrees with bAnsi here (byte-sniffing would
                    // mis-handle non-ASCII Unicode such as a CJK title).
                    if (str->bAnsi)
                    {
                        return cp1252_to_u16(read_string<char>(c.win_emu.memory, str->Buffer, length));
                    }

                    return read_string<char16_t>(c.win_emu.memory, str->Buffer, length / sizeof(char16_t));
                }
            }

            std::array<uint8_t, 2> first_bytes{};
            if (!c.win_emu.memory.try_read_memory(text.value(), first_bytes.data(), first_bytes.size()))
            {
                return {};
            }

            if (first_bytes[0] >= 0x20 && first_bytes[0] < 0x7F && first_bytes[1] == 0)
            {
                return read_string<char16_t>(c.win_emu.memory, text.value());
            }

            if (first_bytes[0] >= 0x20 && first_bytes[0] < 0x7F)
            {
                return cp1252_to_u16(read_string<char>(c.win_emu.memory, text.value()));
            }

            return read_string<char16_t>(c.win_emu.memory, text.value());
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

        hmenu ensure_system_menu(const syscall_context& c, window& win)
        {
            if (win.system_menu_handle != 0 && c.proc.menus.get(win.system_menu_handle) != nullptr)
            {
                return win.system_menu_handle;
            }

            auto [handle, menu_obj] = c.proc.menus.create(c.win_emu.memory);
            menu_obj.handle = handle.bits;
            menu_obj.popup = false;
            menu_obj.init_guest();

            win.system_menu_handle = menu_obj.handle;
            win.guest.access([&](USER_WINDOW& guest_win) { //
                guest_win.spmenu = menu_obj.guest.value();
            });

            return handle.bits;
        }

        std::u16string read_menu_item_text(const syscall_context& c, const EMU_MENUITEMINFO& mi,
                                           const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> item_text)
        {
            if (item_text)
            {
                const auto str = item_text.try_read();
                if (str && str->Buffer != 0 && str->Length != 0)
                {
                    std::u16string result(str->Length / sizeof(char16_t), u'\0');
                    if (c.win_emu.memory.try_read_memory(str->Buffer, result.data(), str->Length))
                    {
                        return result;
                    }
                }
            }

            if (mi.dwTypeData != 0 && mi.cch != 0)
            {
                std::u16string result(mi.cch, u'\0');
                const auto bytes = result.size() * sizeof(char16_t);
                if (c.win_emu.memory.try_read_memory(mi.dwTypeData, result.data(), bytes))
                {
                    const auto terminator = result.find(u'\0');
                    if (terminator != std::u16string::npos)
                    {
                        result.resize(terminator);
                    }
                    return result;
                }
            }

            return {};
        }

        void update_menu_item(const syscall_context& c, menu_item& item, const EMU_MENUITEMINFO& mi,
                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> item_text)
        {
            if ((mi.fMask & MIIM_ID) != 0)
            {
                item.id = mi.wID;
            }
            if ((mi.fMask & MIIM_SUBMENU) != 0)
            {
                item.submenu = mi.hSubMenu;
                if (const auto* submenu = c.proc.menus.get(item.submenu))
                {
                    item.submenu_ptr = submenu->guest.value();
                }
                else
                {
                    item.submenu_ptr = 0;
                }
            }
            if ((mi.fMask & MIIM_FTYPE) != 0)
            {
                item.type = mi.fType;
            }
            if ((mi.fMask & MIIM_STATE) != 0)
            {
                item.state = mi.fState;
            }
            if ((mi.fMask & MIIM_DATA) != 0)
            {
                item.data = mi.dwItemData;
            }
            if ((mi.fMask & MIIM_CHECKMARKS) != 0)
            {
                item.hbmp_checked = mi.hbmpChecked;
                item.hbmp_unchecked = mi.hbmpUnchecked;
            }
            if ((mi.fMask & MIIM_BITMAP) != 0)
            {
                item.hbmp_item = mi.hbmpItem;
            }
            if ((mi.fMask & MIIM_STRING) != 0)
            {
                item.text = read_menu_item_text(c, mi, item_text);
            }
        }
    }

    namespace syscalls
    {
        hdc handle_NtGdiGetDCforBitmap(const syscall_context& c, handle bitmap);
        hdc create_gdi_window_dc(const syscall_context& c, hwnd window);
        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
        BOOL handle_NtGdiFlush(const syscall_context& c);
        gdi_bitmap_surface* get_dc_present_surface(const syscall_context& c, hdc dc, uint32_t& present_handle);
        void draw_system_button_glyph(const syscall_context& c, hdc dc, int x, int y, uint32_t index);
        BOOL handle_NtUserRemoveMenu(const syscall_context& c, hmenu menu, UINT position, UINT flags);

        NTSTATUS handle_NtUserTraceLoggingSendMixedModeTelemetry()
        {
            return STATUS_SUCCESS;
        }

        uint32_t handle_NtUserRegisterWindowMessage(const syscall_context& c,
                                                    const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> message_name)
        {
            if (!message_name)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            const auto raw = message_name.try_read();
            if (!raw || raw->Buffer == 0 || raw->Length == 0 || raw->Length > raw->MaximumLength || (raw->Length & 1) != 0)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            std::u16string name(raw->Length / sizeof(char16_t), u'\0');
            if (!c.win_emu.memory.try_read_memory(raw->Buffer, name.data(), raw->Length))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            const auto message = static_cast<uint32_t>(c.proc.add_or_find_atom(std::move(name)));

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("RegisterWindowMessage atom=#" + std::to_string(message));
            }

            return message;
        }

        uint64_t handle_NtUserGetThreadState(const syscall_context& c, const ULONG routine)
        {
            if (routine == k_thread_state_message_time)
            {
                return c.vcpu.active_thread ? c.vcpu.active_thread->current_message_time : 0;
            }

            if (routine == k_thread_state_dialog_state)
            {
                return c.vcpu.active_thread ? c.vcpu.active_thread->win32k_thread_state : 0;
            }

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

            if (c.proc.is_wow64_process && c.vcpu.active_thread && !c.vcpu.active_thread->win32k_thread_setup_done &&
                !c.vcpu.active_thread->win32k_thread_setup_pending)
            {
                c.vcpu.active_thread->win32k_thread_setup_pending = true;
                dispatch_user_callback(c, callback_id::NtUserGetThreadState, k_client_setup_callback_id);
                return 0;
            }

            return thread_info;
        }

        uint64_t handle_NtUserSetThreadState(const syscall_context& c, const uint64_t value, const uint64_t mask)
        {
            auto* thread = c.vcpu.active_thread;
            if (!thread)
            {
                return 0;
            }

            const auto previous = thread->win32k_thread_state;
            thread->win32k_thread_state = (previous & ~mask) | (value & mask);
            return previous;
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

            if (c.vcpu.active_thread)
            {
                c.vcpu.active_thread->win32k_thread_setup_pending = false;
                c.vcpu.active_thread->win32k_thread_setup_done = true;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUserInitializeClientPfnArrays(const syscall_context& c, const emulator_pointer apfn_client_a,
                                                        const emulator_pointer apfn_client_w, const emulator_pointer apfn_client_worker,
                                                        const emulator_pointer /*hmod_user*/)
        {
            if (c.vcpu.active_thread)
            {
                c.vcpu.active_thread->win32k_thread_setup_pending = false;
                c.vcpu.active_thread->win32k_thread_setup_done = true;
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
            if (thread_id == 0 || (c.vcpu.active_thread && c.vcpu.active_thread->id == thread_id))
            {
                target = c.vcpu.active_thread;
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

        hdc handle_NtUserGetDCEx(const syscall_context& c, const hwnd window, const uint64_t /*clip_region*/, const ULONG /*flags*/)
        {
            return create_gdi_window_dc(c, window);
        }

        hdc handle_NtUserGetDC(const syscall_context& c, const hwnd window)
        {
            return handle_NtUserGetDCEx(c, window, 0, 0);
        }

        hdc handle_NtUserGetWindowDC(const syscall_context& c, const hwnd window)
        {
            return handle_NtUserGetDCEx(c, window, 0, 0);
        }

        hwnd handle_NtUserWindowFromDC(const syscall_context& c, const hdc dc)
        {
            const auto it = c.proc.gdi_dc_states.find(static_cast<uint32_t>(dc));
            if (it == c.proc.gdi_dc_states.end())
            {
                return 0;
            }

            const auto window = it->second.target_window;
            if (window == 0 || !c.proc.windows.get(window))
            {
                return 0;
            }

            return window;
        }

        uint64_t handle_NtUserGetControlBrush(const syscall_context& c, hwnd /*window*/, hdc /*dc*/, uint32_t control_type)
        {
            uint32_t brush_index = k_color_btnface;
            if (control_type == k_ctlcolor_edit || control_type == k_ctlcolor_listbox)
            {
                brush_index = k_color_window;
            }
            else if (control_type == k_ctlcolor_scrollbar)
            {
                brush_index = k_color_scrollbar;
            }

            uint64_t brush = 0;
            c.proc.user_handles.get_server_info().access([&](const USER_SERVERINFO& server_info) {
                if (brush_index < USER_SERVERINFO_BRUSH_SLOT_COUNT)
                {
                    brush = server_info.ahbrSystem[brush_index];
                }
            });

            return brush;
        }

        BOOL handle_NtUserReleaseDC()
        {
            return TRUE;
        }

        hwnd handle_NtUserSetCapture(const syscall_context& c, const hwnd window)
        {
            const auto previous = c.proc.mouse_capture_window;
            if (window == 0 || c.proc.windows.get(window))
            {
                c.proc.mouse_capture_window = window;
            }
            return previous;
        }

        BOOL handle_NtUserReleaseCapture(const syscall_context& c)
        {
            c.proc.mouse_capture_window = 0;
            return TRUE;
        }

        // Games use raw input (RAWINPUTDEVICE with usUsagePage=1/usUsage=2, the generic mouse) for in-game
        // mouse-look: instead of consuming WM_MOUSEMOVE they register here and read relative deltas from the
        // WM_INPUT messages we then synthesize. Record the target window so handle_ui_event knows to deliver
        // raw input; a RIDEV_REMOVE entry for the mouse unregisters it.
        BOOL handle_NtUserRegisterRawInputDevices(const syscall_context& c, const emulator_pointer devices, const uint32_t device_count,
                                                  const uint32_t size)
        {
            if (devices == 0 || size < sizeof(RAWINPUTDEVICE32))
            {
                return FALSE;
            }

            for (uint32_t i = 0; i < device_count; ++i)
            {
                RAWINPUTDEVICE32 device{};
                if (!c.emu.try_read_memory(devices + static_cast<uint64_t>(i) * size, &device, sizeof(device)))
                {
                    return FALSE;
                }

                constexpr uint16_t hid_usage_page_generic = 0x01;
                constexpr uint16_t hid_usage_generic_mouse = 0x02;
                constexpr uint16_t hid_usage_generic_keyboard = 0x06;
                if (device.usUsagePage != hid_usage_page_generic ||
                    (device.usUsage != hid_usage_generic_mouse && device.usUsage != hid_usage_generic_keyboard))
                {
                    continue;
                }

                // hwndTarget == 0 is the valid "follow keyboard focus" mode; keep it as-is and resolve the
                // destination at delivery time rather than freezing whatever window is foreground right now
                // (registration often happens at startup before any window has become foreground).
                const bool remove = (device.dwFlags & RIDEV_REMOVE) != 0;
                if (device.usUsage == hid_usage_generic_mouse)
                {
                    c.proc.raw_mouse_registered = !remove;
                    c.proc.raw_mouse_target = remove ? hwnd{} : device.hwndTarget;
                }
                else
                {
                    c.proc.raw_keyboard_registered = !remove;
                    c.proc.raw_keyboard_target = remove ? hwnd{} : device.hwndTarget;
                }
            }

            return TRUE;
        }

        ULONG handle_NtUserGetRawInputDeviceList(const syscall_context& c, const emulator_pointer devices,
                                                 const emulator_pointer device_count, const uint32_t size)
        {
            constexpr uint32_t required_count = 2;
            constexpr uint32_t list32_size = 8;
            constexpr uint32_t list64_size = 16;
            constexpr uint64_t mouse_handle = 0x10001;
            constexpr uint64_t keyboard_handle = 0x10002;

            if (device_count == 0 || (size != list32_size && size != list64_size))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return static_cast<ULONG>(-1);
            }

            uint32_t capacity = 0;
            if (!c.emu.try_read_memory(device_count, &capacity, sizeof(capacity)))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return static_cast<ULONG>(-1);
            }

            if (devices == 0)
            {
                c.emu.write_memory(device_count, &required_count, sizeof(required_count));
                return 0;
            }

            if (capacity < required_count)
            {
                c.emu.write_memory(device_count, &required_count, sizeof(required_count));
                set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                return static_cast<ULONG>(-1);
            }

            if (size == list32_size)
            {
                struct raw_input_device_list32
                {
                    uint32_t device;
                    uint32_t type;
                };

                const std::array device_list = {
                    raw_input_device_list32{.device = static_cast<uint32_t>(mouse_handle), .type = RIM_TYPEMOUSE},
                    raw_input_device_list32{.device = static_cast<uint32_t>(keyboard_handle), .type = RIM_TYPEKEYBOARD},
                };
                c.emu.write_memory(devices, device_list.data(), sizeof(device_list));
            }
            else
            {
                struct raw_input_device_list64
                {
                    uint64_t device;
                    uint32_t type;
                };

                const std::array device_list = {
                    raw_input_device_list64{.device = mouse_handle, .type = RIM_TYPEMOUSE},
                    raw_input_device_list64{.device = keyboard_handle, .type = RIM_TYPEKEYBOARD},
                };
                c.emu.write_memory(devices, device_list.data(), sizeof(device_list));
            }

            c.emu.write_memory(device_count, &required_count, sizeof(required_count));
            return required_count;
        }

        ULONG handle_NtUserGetRawInputDeviceInfo(const syscall_context& c, const handle device, const uint32_t command,
                                                 const emulator_pointer data, const emulator_pointer size)
        {
            constexpr uint64_t mouse_handle = 0x10001;
            constexpr uint64_t keyboard_handle = 0x10002;
            constexpr uint32_t ridi_device_name = 0x20000007;
            constexpr uint32_t ridi_device_info = 0x2000000B;
            constexpr std::u16string_view mouse_name = u"\\\\?\\HID#SOGEN_MOUSE#0001#{378de44c-56ef-11d1-bc8c-00a0c91405dd}";
            constexpr std::u16string_view keyboard_name = u"\\\\?\\HID#SOGEN_KEYBOARD#0001#{884b96c3-56ef-11d1-bc8c-00a0c91405dd}";

            if (size == 0 || (device != mouse_handle && device != keyboard_handle))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return static_cast<ULONG>(-1);
            }

            uint32_t capacity = 0;
            if (!c.emu.try_read_memory(size, &capacity, sizeof(capacity)))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return static_cast<ULONG>(-1);
            }

            if (command == ridi_device_name)
            {
                const auto name = device == mouse_handle ? mouse_name : keyboard_name;
                const auto required_characters = static_cast<uint32_t>(name.size() + 1);
                if (data == 0)
                {
                    c.emu.write_memory(size, &required_characters, sizeof(required_characters));
                    return 0;
                }
                if (capacity < required_characters)
                {
                    c.emu.write_memory(size, &required_characters, sizeof(required_characters));
                    set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                    return static_cast<ULONG>(-1);
                }

                c.emu.write_memory(data, name.data(), name.size() * sizeof(char16_t));
                const char16_t terminator = 0;
                c.emu.write_memory(data + name.size() * sizeof(char16_t), &terminator, sizeof(terminator));
                c.emu.write_memory(size, &required_characters, sizeof(required_characters));
                return static_cast<ULONG>(name.size());
            }

            if (command == ridi_device_info)
            {
                struct raw_input_device_info
                {
                    uint32_t structure_size;
                    uint32_t type;
                    std::array<uint32_t, 6> details;
                };
                static_assert(sizeof(raw_input_device_info) == 32);

                constexpr uint32_t required_bytes = sizeof(raw_input_device_info);
                if (data == 0)
                {
                    c.emu.write_memory(size, &required_bytes, sizeof(required_bytes));
                    return 0;
                }
                if (capacity < required_bytes)
                {
                    c.emu.write_memory(size, &required_bytes, sizeof(required_bytes));
                    set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                    return static_cast<ULONG>(-1);
                }

                raw_input_device_info info{};
                info.structure_size = required_bytes;
                if (device == mouse_handle)
                {
                    info.type = RIM_TYPEMOUSE;
                    info.details = {1, 3, 100, 0, 0, 0};
                }
                else
                {
                    info.type = RIM_TYPEKEYBOARD;
                    info.details = {4, 0, 1, 12, 3, 101};
                }

                c.emu.write_memory(data, &info, sizeof(info));
                c.emu.write_memory(size, &required_bytes, sizeof(required_bytes));
                return required_bytes;
            }

            set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
            return static_cast<ULONG>(-1);
        }

        // GetRawInputData fetches the payload referenced by a WM_INPUT message's lParam (an HRAWINPUT token we
        // minted in handle_ui_event). We only synthesize relative mouse motion, so reconstruct a RAWINPUT whose
        // mouse delta is the {dx, dy} stored for the token. uiCommand selects the header-only (RID_HEADER) or
        // full (RID_INPUT) payload; passing pData==NULL is the standard size query.
        //
        // The RAWMOUSE body follows the header with no extra padding, so it sits at offset == cbSizeHeader. A
        // WoW64 (32-bit) guest on 64-bit Windows gets the *64-bit* RAWINPUTHEADER (24 bytes: 8-byte hDevice +
        // wParam), not the 16-byte 32-bit one -- the well-known WoW64 raw-input quirk -- so honor whatever
        // cbSizeHeader the caller passes and place the mouse body there, instead of assuming a fixed layout.
        uint32_t handle_NtUserGetRawInputData(const syscall_context& c, const emulator_pointer raw_input, const uint32_t command,
                                              const emulator_pointer data, const emulator_object<uint32_t> size_ptr,
                                              const uint32_t header_size)
        {
            constexpr auto failure = static_cast<uint32_t>(-1);
            constexpr uint32_t max_header_size = 0x20;

            if (!size_ptr || header_size < sizeof(RAWINPUTHEADER32) || header_size > max_header_size)
            {
                return failure;
            }

            const auto token = static_cast<uint32_t>(raw_input);
            const auto entry = c.proc.raw_inputs.find(token);
            if (entry == c.proc.raw_inputs.end())
            {
                return failure;
            }

            const auto& payload = entry->second;

            // The body is a RAWKEYBOARD or RAWMOUSE depending on what the token carried.
            const uint32_t body_size = payload.keyboard ? sizeof(RAWKEYBOARD32) : sizeof(RAWMOUSE32);
            const uint32_t dw_type = payload.keyboard ? RIM_TYPEKEYBOARD : RIM_TYPEMOUSE;

            const uint32_t total = header_size + body_size;
            const uint32_t required = (command == RID_HEADER) ? header_size : total;

            if (data == 0)
            {
                size_ptr.write(required);
                return 0;
            }

            if (size_ptr.read() < required)
            {
                return failure;
            }

            // dwType @0 and dwSize @4 are identical across header layouts; hDevice/wParam stay zeroed, so only
            // the overall header size (which shifts the body) matters.
            std::array<uint8_t, max_header_size + sizeof(RAWMOUSE32)> buffer{};
            const uint32_t dw_size = total;
            std::memcpy(buffer.data() + 0, &dw_type, sizeof(dw_type));
            std::memcpy(buffer.data() + 4, &dw_size, sizeof(dw_size));
            if (command != RID_HEADER)
            {
                if (payload.keyboard)
                {
                    RAWKEYBOARD32 keyboard{};
                    keyboard.MakeCode = payload.scan_code;
                    const bool key_release = payload.key_message == WM_KEYUP || payload.key_message == WM_SYSKEYUP;
                    keyboard.Flags =
                        static_cast<uint16_t>((key_release ? RI_KEY_BREAK : RI_KEY_MAKE) | (payload.key_extended ? RI_KEY_E0 : 0));
                    keyboard.VKey = payload.vkey;
                    keyboard.Message = payload.key_message;
                    std::memcpy(buffer.data() + header_size, &keyboard, sizeof(keyboard));
                }
                else
                {
                    RAWMOUSE32 mouse{};
                    mouse.usFlags = MOUSE_MOVE_RELATIVE;
                    // RAWMOUSE has a usButtonFlags/usButtonData union packed into ulButtons. The high
                    // word carries wheel delta for RI_MOUSE_WHEEL/RI_MOUSE_HWHEEL.
                    mouse.ulButtons =
                        static_cast<uint32_t>(payload.mouse_buttons) | (static_cast<uint32_t>(payload.mouse_button_data) << 16);
                    mouse.lLastX = payload.dx;
                    mouse.lLastY = payload.dy;
                    std::memcpy(buffer.data() + header_size, &mouse, sizeof(mouse));
                }
                c.proc.raw_inputs.erase(entry);
            }

            c.emu.write_memory(data, buffer.data(), required);
            return required;
        }

        BOOL handle_NtUserDefSetText(const syscall_context& c, const hwnd window, const emulator_object<LARGE_STRING> text)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            update_window_title(c, *win, read_def_set_text_string(c, text));
            return TRUE;
        }

        // user32 builds the classic checkbox/radio glyph bitmaps (FUN_18000b440 in user32) by asking
        // win32k for each OEM control bitmap's dimensions via NtUserGetOemBitmapSize(id, SIZE*), filling
        // { LONG cx; LONG cy; }. We don't host the real system bitmaps, but the size is still needed so
        // button layout/paint can proceed instead of aborting on an unimplemented syscall. The classic
        // check/radio glyph is 13x13 at 96 DPI; report that for every id.
        BOOL handle_NtUserGetOemBitmapSize(const syscall_context& c, const uint32_t /*bitmap_id*/, const emulator_pointer size_ptr)
        {
            if (size_ptr == 0)
            {
                return FALSE;
            }

            constexpr std::array<int32_t, 2> size = {13, 13}; // { cx, cy }
            c.emu.write_memory(size_ptr, size.data(), sizeof(size));
            return TRUE;
        }

        // user32 stores per-window state bits (button checked/pushed/focus, etc.) in the window object's
        // state bytes starting at WND offset 0x10. SetWindowState/ClearWindowState set/clear them: the
        // high byte of `flags` selects the state byte (0x10 + (flags >> 8)) and the low byte is the mask.
        // The client (e.g. the button wndproc) reads these bytes back from the same shared window object
        // when painting, so faithfully OR/AND-ing the mask keeps client-side state consistent.
        BOOL apply_window_state(const syscall_context& c, const hwnd window, const uint32_t flags, const bool set)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            const auto byte_address = win->guest.value() + 0x10 + ((flags >> 8) & 0xFF);
            const auto mask = static_cast<uint8_t>(flags & 0xFF);

            uint8_t state = 0;
            c.win_emu.memory.try_read_memory(byte_address, &state, sizeof(state));
            state = set ? static_cast<uint8_t>(state | mask) : static_cast<uint8_t>(state & ~mask);
            c.win_emu.memory.write_memory(byte_address, &state, sizeof(state));
            return TRUE;
        }

        BOOL handle_NtUserSetWindowState(const syscall_context& c, const hwnd window, const uint32_t flags)
        {
            return apply_window_state(c, window, flags, true);
        }

        BOOL handle_NtUserClearWindowState(const syscall_context& c, const hwnd window, const uint32_t flags)
        {
            return apply_window_state(c, window, flags, false);
        }

        BOOL handle_NtUserDisableProcessWindowsGhosting(const syscall_context& /*c*/)
        {
            // Window ghosting is a desktop-compositor feature with no meaning in the emulator; accept the
            // request so callers (e.g. game engine startup) proceed.
            return TRUE;
        }

        BOOL handle_NtUserBitBltSysBmp(const syscall_context& c, const hdc dc, const int x, const int y, const uint32_t bitmap_index)
        {
            (void)handle_NtGdiFlush(c);
            draw_system_button_glyph(c, dc, x, y, bitmap_index);
            return TRUE;
        }

        BOOL handle_NtUserGetClientRect(const syscall_context& c, const hwnd window, const emulator_pointer rect_ptr)
        {
            if (rect_ptr == 0)
            {
                return FALSE;
            }
            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }
            const auto cr = get_client_rect(*win);
            c.emu.write_memory(rect_ptr, &cr, sizeof(cr));
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

        BOOL handle_NtUserEndPaint(const syscall_context& c, const hwnd window, const emulator_object<EMU_PAINTSTRUCT> paint_struct)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            if (paint_struct)
            {
                const auto ps = paint_struct.read();
                (void)handle_NtGdiFlush(c);

                // Present the surface the guest just painted into. For child controls this resolves to the owning
                // top-level window's shared surface, keyed by that top-level host window handle.
                uint32_t present_handle = 0;
                if (auto* surface = get_dc_present_surface(c, ps.paint_hdc, present_handle);
                    surface && present_handle != 0 && surface->width > 0 && surface->height > 0 && !surface->pixels.empty())
                {
                    c.win_emu.ui().present_surface(present_handle,
                                                   ui_surface_desc{.width = static_cast<int>(surface->width),
                                                                   .height = static_cast<int>(surface->height),
                                                                   .stride = static_cast<int>(surface->width * sizeof(uint32_t)),
                                                                   .format = ui_surface_format::bgra8,
                                                                   .pixels = surface->pixels.data()});
                }

                // BeginPaint allocated a fresh GDI DC (handle table entry + DC_ATTR block) via
                // GetDCEx; fully delete it here so repeated repaints don't leak GDI handles.
                (void)handle_NtGdiDeleteObjectApp(c, static_cast<uint32_t>(ps.paint_hdc));
            }

            validate_window(*win);
            return TRUE;
        }

        BOOL handle_NtUserGetCursorPos(const syscall_context& c, const emulator_pointer point_ptr)
        {
            if (point_ptr == 0)
            {
                return FALSE;
            }

            // POINT is { LONG x; LONG y; }. Report the last tracked cursor position in screen coordinates.
            const std::array<int32_t, 2> pt = {c.proc.cursor_x, c.proc.cursor_y};
            c.emu.write_memory(point_ptr, pt.data(), sizeof(pt));
            return TRUE;
        }

        BOOL handle_NtUserGetCursorInfo(const syscall_context& c, const emulator_object<EMU_CURSORINFO> cursor_info)
        {
            if (!cursor_info)
            {
                return FALSE;
            }

            cursor_info.write({
                .cbSize = sizeof(EMU_CURSORINFO),
                .flags = c.proc.cursor_show_count >= 0 && c.proc.cursor_shape_visible ? 1u : 0u,
                .hCursor = c.proc.current_cursor,
                .ptScreenPos = {.x = c.proc.cursor_x, .y = c.proc.cursor_y},
            });
            return TRUE;
        }

        BOOL handle_NtUserGetClipCursor(const syscall_context& c, const emulator_pointer rect_ptr)
        {
            if (rect_ptr == 0)
            {
                return FALSE;
            }

            // TODO: This should return the clip cursor. For simplicity, we return a best effort derived from the screen size.
            RECT rect{0, 0, 1920, 1080};
            const auto display_info = c.proc.user_handles.get_display_info().read();
            if (const emulator_object<USER_MONITOR> monitor_obj(c.emu, display_info.pPrimaryMonitor); monitor_obj)
            {
                rect = monitor_obj.read().rcMonitor;
            }

            c.emu.write_memory(rect_ptr, &rect, sizeof(rect));
            return TRUE;
        }

        // user32 coordinate helpers (ScreenToClient/ClientToScreen/MapWindowPoints) call this to convert a
        // point between per-monitor DPI spaces before applying the window-origin offset they do themselves.
        // The emulated desktop is a single 96-DPI space, so the conversion is the identity: leave the point.
        BOOL handle_NtUserTransformPoint(const syscall_context& /*c*/, const emulator_pointer /*point*/, const uint32_t /*from_dpi*/,
                                         const uint32_t /*to_dpi*/, const uint32_t /*flags*/)
        {
            return TRUE;
        }

        // SetCursorPos moves the cursor; games use it to recenter the pointer. Track the new position so a
        // following GetCursorPos reflects it (and relative-motion deltas compute correctly).
        BOOL handle_NtUserSetCursorPos(const syscall_context& c, const int32_t x, const int32_t y)
        {
            c.proc.cursor_x = x;
            c.proc.cursor_y = y;
            // Warp the host cursor to match, so the next host mouse-motion event measures movement from the
            // recentered position instead of overwriting it with the stale host location (relative-mouse look).
            if (c.proc.foreground_window != 0)
            {
                c.win_emu.ui().set_cursor_position(c.proc.foreground_window, x, y);
            }
            return TRUE;
        }

        // ClipCursor confines the cursor to a rectangle. The emulated cursor is driven by the host window
        // and never escapes it, so confinement is a no-op; just report success.
        BOOL handle_NtUserClipCursor(const syscall_context& /*c*/, const emulator_pointer /*rect*/)
        {
            return TRUE;
        }

        // GetKeyState / GetAsyncKeyState report whether a virtual key (or mouse button) is currently down.
        // Games poll these for in-game input instead of consuming WM_KEYDOWN messages. GetKeyState reports the
        // high down bit; GetAsyncKeyState additionally returns the low pressed-since-last-query bit.
        uint32_t handle_NtUserGetKeyState(const syscall_context& c, const int32_t virtual_key)
        {
            return (c.proc.key_state[static_cast<uint32_t>(virtual_key) & 0xFF] & 0x80) ? 0x8000u : 0u;
        }

        uint32_t handle_NtUserGetAsyncKeyState(const syscall_context& c, const int32_t virtual_key)
        {
            const auto key = static_cast<uint32_t>(virtual_key) & 0xFF;
            uint32_t result = (c.proc.key_state[key] & 0x80) ? 0x8000u : 0u;
            if (c.proc.async_key_state[key] != 0)
            {
                result |= 0x0001u;
                c.proc.async_key_state[key] = 0;
            }
            return result;
        }

        // The host pointer is shown only when the display count is non-negative and the current cursor has a
        // visible shape; mirror that to the UI so a captured (hidden + recentered) cursor stops flickering.
        void apply_cursor_visibility(const syscall_context& c)
        {
            c.win_emu.ui().set_cursor_visibility(c.proc.cursor_show_count >= 0 && c.proc.cursor_shape_visible);
        }

        // ShowCursor(bShow) adjusts the cursor display counter (+1 to show, -1 to hide) and returns the new
        // value. Games spin on it to drive the count to a target (e.g. IN_ActivateMouse hides the cursor), so
        // it must actually track and return the running count or those loops never terminate.
        int32_t handle_NtUserShowCursor(const syscall_context& c, const BOOL show)
        {
            c.proc.cursor_show_count += show ? 1 : -1;
            apply_cursor_visibility(c);
            return c.proc.cursor_show_count;
        }

        hcursor handle_NtUserSetCursor(const syscall_context& c, const hcursor cursor)
        {
            // SetCursor(NULL) removes the cursor shape (hides it); a non-null handle restores it.
            c.proc.cursor_shape_visible = cursor != 0;
            apply_cursor_visibility(c);
            return std::exchange(c.proc.current_cursor, cursor);
        }

        hcursor handle_NtUserGetCursor(const syscall_context& c)
        {
            return c.proc.current_cursor;
        }

        hicon handle_NtUserCreateEmptyCursorObject()
        {
            return make_pseudo_handle(0x100, handle_types::reserved).bits;
        }

        BOOL handle_NtUserSetCursorIconData()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetCursorIconDataEx()
        {
            return TRUE;
        }

        BOOL handle_NtUserGetRequiredCursorSizes()
        {
            return TRUE;
        }

        NTSTATUS handle_NtUserFindExistingCursorIcon()
        {
            return STATUS_NOT_SUPPORTED;
        }

        BOOL handle_NtUserDestroyCursor(const syscall_context&, const hicon icon, const DWORD /*flags*/)
        {
            return icon != 0 ? TRUE : FALSE;
        }

        hicon handle_NtUserGetCursorFrameInfo(const syscall_context&, const hicon icon, const UINT /*frame*/,
                                              const emulator_object<uint32_t> rate_jiffies, const emulator_object<uint32_t> frame_count)
        {
            if (icon == 0)
            {
                return 0;
            }

            rate_jiffies.write_if_valid(0);
            frame_count.write_if_valid(1);
            return icon;
        }

        BOOL handle_NtUserGetIconSize(const syscall_context&, const hicon icon, const UINT /*frame*/, const emulator_object<int> cx,
                                      const emulator_object<int> cy)
        {
            if (icon == 0 || !cx || !cy)
            {
                return FALSE;
            }

            cx.write(32);
            cy.write(64);
            return TRUE;
        }

        BOOL handle_NtUserGetIconInfo(const syscall_context& c, const hicon icon, const emulator_object<EMU_ICONINFO> icon_info,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> inst_name,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> res_name,
                                      const emulator_object<uint32_t> bpp, const BOOL /*internal*/)
        {
            if (icon == 0 || !icon_info)
            {
                set_guest_last_error(c, 1414); // ERROR_INVALID_CURSOR_HANDLE
                return FALSE;
            }

            // The emulator's icons/cursors are bare pseudo-handles with no backing pixel data, so report a
            // standard 32x32 icon with a centered hotspot and no mask/color bitmaps.
            const EMU_ICONINFO info{
                .fIcon = TRUE,
                .xHotspot = 16,
                .yHotspot = 16,
                .hbmMask = 0,
                .hbmColor = 0,
            };
            icon_info.write(info);

            const auto clear_name = [&](const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>>& name) {
                if (name)
                {
                    auto value = name.read();
                    value.Length = 0;
                    name.write(value);
                }
            };
            clear_name(inst_name);
            clear_name(res_name);

            bpp.write_if_valid(32);
            return TRUE;
        }

        BOOL handle_NtUserMessageBeep()
        {
            return TRUE;
        }

        // Minimal stub: report success so icon/static (SS_ICON) paint completes. Rendering the actual
        // system icon pixels is a separate task; for now the icon area is left unpainted.
        BOOL handle_NtUserDrawIconEx(const syscall_context&, const hdc /*dc*/, const int /*x*/, const int /*y*/, const hicon icon,
                                     const int /*cx*/, const int /*cy*/, const UINT /*istep*/, const uint64_t /*flicker_brush*/,
                                     const UINT /*di_flags*/)
        {
            return icon != 0 ? TRUE : FALSE;
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

        NTSTATUS handle_NtUserBuildHwndList(const syscall_context& c, const hdesk /*desktop*/, const hwnd hwnd_next, const BOOL children,
                                            const BOOL /*remove_immersive*/, const DWORD thread_id, const UINT hwnd_max,
                                            const emulator_pointer hwnd_list, const emulator_object<UINT> hwnd_needed)
        {
            if (!hwnd_needed)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const hwnd desktop_window = c.proc.default_desktop_window_handle.bits;
            std::vector<hwnd> handles{};

            for (const auto& entry : c.proc.windows)
            {
                const auto& win = entry.second;

                if (thread_id != 0 && win.thread_id != thread_id)
                {
                    continue;
                }

                if (children)
                {
                    if (hwnd_next == 0)
                    {
                        if (win.handle == desktop_window || win.parent_handle != desktop_window)
                        {
                            continue;
                        }
                    }
                    else
                    {
                        hwnd current = win.parent_handle;
                        bool is_descendant = false;
                        for (size_t guard = 0; current != 0 && guard < c.proc.windows.size(); ++guard)
                        {
                            if (current == hwnd_next)
                            {
                                is_descendant = true;
                                break;
                            }

                            const auto* parent = c.proc.windows.get(current);
                            current = parent != nullptr ? parent->parent_handle : 0;
                        }

                        if (!is_descendant)
                        {
                            continue;
                        }
                    }
                }
                else
                {
                    if (win.handle == desktop_window || win.parent_handle != desktop_window)
                    {
                        continue;
                    }
                }

                handles.push_back(win.handle);
            }

            handles.push_back(0);
            hwnd_needed.write(static_cast<UINT>(handles.size()));

            if (hwnd_max < handles.size())
            {
                return STATUS_BUFFER_TOO_SMALL;
            }

            if (hwnd_list)
            {
                c.emu.write_memory(hwnd_list, handles.data(), handles.size() * sizeof(handles[0]));
            }

            return STATUS_SUCCESS;
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
            return 1;
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

        int handle_NtUserGetClassName(const syscall_context& c, const hwnd win_hwnd, const BOOL /*real*/,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name)
        {
            const auto* wnd = c.proc.windows.get(win_hwnd);
            if (!wnd)
            {
                set_guest_last_error(c, 6); // ERROR_INVALID_HANDLE
                return 0;
            }
            const auto& name = wnd->class_name;
            const size_t name_length_bytes = name.size() * sizeof(char16_t);

            bool too_small = false;
            size_t copied_chars = 0;

            class_name.access([&](UNICODE_STRING<EmulatorTraits<Emu64>>& str) {
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
                    c.emu.write_memory(str.Buffer, name.data(), copy_bytes);
                }

                constexpr char16_t terminator = 0;
                c.emu.write_memory(str.Buffer + copy_bytes, &terminator, sizeof(terminator));

                str.Length = static_cast<USHORT>(copy_bytes);
                copied_chars = copy_bytes / sizeof(char16_t);
            });

            if (too_small)
            {
                set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                return 0;
            }

            return static_cast<int>(copied_chars);
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
                                         const emulator_object<LARGE_STRING> window_name, const DWORD style, int x, int y, int width,
                                         int height, const hwnd parent, const hmenu menu, const hinstance instance, const pointer l_param,
                                         const DWORD /*flags*/, const pointer /*acbi_buffer*/)
        {
            constexpr int cw_usedefault = static_cast<int>(0x80000000);
            if (x == cw_usedefault)
            {
                x = 64;
                y = (y == cw_usedefault) ? 64 : y;
            }
            else if (y == cw_usedefault)
            {
                y = 64;
            }
            if (width == cw_usedefault)
            {
                width = 640;
                height = (height == cw_usedefault) ? 480 : height;
            }
            else if (height == cw_usedefault)
            {
                height = 480;
            }

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

            const bool is_message_only = parent == EMU_HWND_MESSAGE;
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

            auto [handle, win] = c.proc.windows.create(c.win_emu.memory);
            win.ex_style = ex_style;
            win.style = style;
            win.x = x;
            win.y = y;
            win.width = width;
            win.height = height;
            win.thread_id = c.thread().id;
            win.handle = handle.bits;
            if (!is_message_only)
            {
                win.parent_handle = has_child_parent ? parent : c.proc.default_desktop_window_handle.bits;
                win.owner_handle = has_owner ? parent : 0;
            }
            win.class_name = cls_name;
            const auto normalized_class = normalize_builtin_window_class_name(cls_name);
            // Builtin controls use the wide (apfnClientW) wndproc (expect UTF-16); app classes keep
            // their registered proc. Drives the WM_SETTEXT re-encoding so a wide proc never gets ANSI.
            win.unicode_proc = is_builtin_window_class_name(normalized_class);
            const auto skip_create_messages = false;
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
                guest_win.rcWindow = {.left = x, .top = y, .right = x + width, .bottom = y + height};
                // The client rect is the window minus its non-client frame (see window::nonclient_border). The
                // guest reads rcClient client-side to size its render target/backbuffer, so seeding it with the
                // outer rect here makes a framed window render 2px too large and rescale (softening the frame).
                guest_win.rcClient = {.left = x, .top = y, .right = x + win.client_width(), .bottom = y + win.client_height()};
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
                // Control id offset is build-specific: Win11 reads wID (WND+0x140), Server 2022 reads
                // spmenu (WND+0x98). Populate both so builtin wndprocs emit the right WM_COMMAND id.
                guest_win.wID = has_child_parent ? menu : 0;
                if (has_child_parent)
                {
                    guest_win.spmenu = menu;
                }
                guest_win.windowBand = 1; // ZBID_DESKTOP
                guest_win.dpiContext = USER_DEFAULT_DPI_CONTEXT;
                guest_win.fnid = get_builtin_window_fnid(normalized_class);
                guest_win.threadId = win.thread_id;
                guest_win.processId = process_context::process_id;

                win.host_surface_window = !is_message_only;

                if (wnd_class->cbWndExtra > 0)
                {
                    const auto extra_size = static_cast<size_t>(page_align_up(wnd_class->cbWndExtra));
                    guest_win.pExtraBytes = c.win_emu.memory.allocate_memory(extra_size, memory_permission::read_write);
                }
            });
            write_guest_window_text(c, win, win.name);

            if (has_child_parent && parent_win && parent_win->guest.value() != 0)
            {
                const auto child_ptr = win.guest.value();
                uint64_t first_child = 0;

                parent_win->guest.access([&](USER_WINDOW& parent_guest) {
                    first_child = parent_guest.spwndChild;

                    if (first_child == 0)
                    {
                        parent_guest.spwndChild = child_ptr;
                    }
                });

                if (first_child != 0)
                {
                    uint64_t current = first_child;

                    // Guard against corrupted/cyclic child chains.
                    for (size_t guard = 0; current != 0 && guard < c.proc.windows.size(); ++guard)
                    {
                        uint64_t next = 0;
                        bool appended = false;

                        emulator_object<USER_WINDOW> current_win_obj{c.emu, current};
                        current_win_obj.access([&](USER_WINDOW& current_guest) {
                            next = current_guest.spwndNext;

                            if (next == 0)
                            {
                                current_guest.spwndNext = child_ptr;
                                appended = true;
                            }
                        });

                        if (appended)
                        {
                            win.guest.access([&](USER_WINDOW& guest_win) {
                                guest_win.spwndPrev = current;
                                guest_win.spwndNext = 0;
                            });

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
                    .client_insets = get_host_ui_client_insets(win),
                    .class_name = std::u16string{normalize_builtin_window_class_name(cls_name)},
                    .title = win.name,
                    .style = style,
                    .ex_style = ex_style,
                    .control_id = has_child_parent ? static_cast<uint32_t>(menu) : 0,
                    .visible = (style & WS_VISIBLE) != 0,
                    .enabled = (style & WS_DISABLED) == 0,
                    .top_level = !has_child_parent,
                });

                if (has_child_parent && parent_win && (style & WS_VISIBLE) != 0)
                {
                    invalidate_window(c, win);
                }
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
                {.message = WM_CREATE, .wParam = 0, .lParam = state.create_struct_alloc.address()},
                {.message = WM_NCCALCSIZE, .wParam = 0, .lParam = state.window_rect_alloc.address()},
                {.message = WM_NCCREATE, .wParam = 0, .lParam = state.create_struct_alloc.address()},
                {.message = WM_GETMINMAXINFO, .wParam = 0, .lParam = state.min_max_info_alloc.address()},
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
                    {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_SETFOCUS, .wParam = 0, .lParam = 0},
                    {.message = WM_ACTIVATE, .wParam = 1, .lParam = 0},
                    {.message = WM_NCACTIVATE, .wParam = 1, .lParam = 0},
                    {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address()},
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

            if (s.window_pos_alloc)
            {
                c.emu.pop_stack(s.window_pos_alloc);
            }

            c.emu.pop_stack(s.min_max_info_alloc);
            c.emu.pop_stack(s.window_rect_alloc);
            c.emu.pop_stack(s.create_struct_alloc);

            return s.handle;
        }

        BOOL handle_NtUserDestroyWindow(const syscall_context& c, const hwnd window)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            if (win->thread_id != c.vcpu.active_thread->id)
            {
                return FALSE;
            }

            window_destroy_state state{};
            window_destroy_orchestrator{state, c}.start(*win);
            return advance_window_destroy(c, state);
        }

        BOOL completion_NtUserDestroyWindow(const syscall_context& c, const hwnd /*window*/)
        {
            auto& s = c.get_completion_state<window_destroy_state>();
            return advance_window_destroy(c, s);
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

        uint64_t handle_NtUserGetProp(const syscall_context& c, const hwnd window, const uint16_t atom)
        {
            const auto* win = c.proc.windows.get(window);
            const auto prop = c.proc.get_atom_name(atom);

            if (!win || !prop)
            {
                return 0;
            }

            const auto entry = win->props.find(*prop);
            return entry != win->props.end() ? entry->second : 0;
        }

        uint64_t handle_NtUserGetProp2(const syscall_context& c, const hwnd window,
                                       const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> str)
        {
            const auto* win = c.proc.windows.get(window);
            if (!win || !str)
            {
                return 0;
            }

            const auto prop = read_unicode_string_or_atom(c, str);
            if (prop.empty())
            {
                return 0;
            }

            const auto entry = win->props.find(prop);
            return entry != win->props.end() ? entry->second : 0;
        }

        uint64_t handle_NtUserRemoveProp(const syscall_context& c, const hwnd window, const uint16_t atom)
        {
            auto* win = c.proc.windows.get(window);
            const auto prop = c.proc.get_atom_name(atom);
            if (!win || !prop)
            {
                return 0;
            }

            const auto entry = win->props.find(*prop);
            if (entry == win->props.end())
            {
                return 0;
            }

            const auto data = entry->second;
            win->props.erase(entry);
            return data;
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

            if (win->thread_id != c.vcpu.active_thread->id)
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
                if (want_visible && win->is_dialog())
                {
                    sync_child_control_titles_from_guest(c, *win);
                }
                c.win_emu.ui().set_window_visible(hwnd, want_visible);
            }

            if (want_visible)
            {
                const auto move_lparam = static_cast<uint64_t>(((win->y & 0xFFFF) << 16) | (win->x & 0xFFFF));
                const auto size_lparam = static_cast<uint64_t>(((win->height & 0xFFFF) << 16) | (win->width & 0xFFFF));

                state.message_queue = {
                    {.message = WM_MOVE, .wParam = 0, .lParam = move_lparam},
                    {.message = WM_SIZE, .wParam = 0, .lParam = size_lparam},
                    {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_SETFOCUS, .wParam = 0, .lParam = 0},
                    {.message = WM_ACTIVATE, .wParam = 1, .lParam = 0},
                    {.message = WM_NCACTIVATE, .wParam = TRUE, .lParam = 0},
                    {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address()},
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
                    {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_WINDOWPOSCHANGING, .wParam = 0, .lParam = state.window_pos_alloc.address()},
                    {.message = WM_SHOWWINDOW, .wParam = FALSE, .lParam = 0},
                };

                win->style &= ~WS_VISIBLE;
            }

            win->guest.access([&](USER_WINDOW& guest_win) { //
                guest_win.dwStyle = win->style;
            });

            if (want_visible)
            {
                // Now that this window is visible, repaint it and any visible child controls that
                // were created (and skipped painting) while their ancestor chain was still hidden.
                invalidate_window_tree(c, *win);
            }

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

            c.emu.pop_stack(s.window_pos_alloc);

            return s.was_visible ? TRUE : FALSE;
        }

        uint64_t handle_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t w_param,
                                          const uint64_t l_param, const uint64_t result_info, const DWORD type, const BOOL ansi)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            if (type == FNID_DEFWINDOW)
            {
                const auto result = handle_default_window_proc_message(c, *win, msg, w_param, l_param, ansi);
                return write_message_call_result(c, result_info, result) ? TRUE : FALSE;
            }

            if (win->thread_id != c.vcpu.active_thread->id)
            {
                // TODO: This is a bit incorrect. We're supposed to wait until the message is received, but this is fine for a first
                //       minimal version.
                if (type == FNID_SENDMESSAGECALLBACK)
                {
                    if (auto* t = c.proc.find_thread_by_id(win->thread_id))
                    {
                        sogen::msg queued_message{};
                        queued_message.window = hwnd;
                        queued_message.message = msg;
                        queued_message.wParam = w_param;
                        queued_message.lParam = l_param;
                        t->post_message(c.win_emu, queued_message);
                        return TRUE;
                    }
                }

                return 0;
            }

            uint64_t dispatch_l_param = l_param;
            uint64_t scratch_text = 0;

            if (msg == WM_SETTEXT)
            {
                if (l_param == 0)
                {
                    update_window_title(c, *win, {});
                }
                else if (ansi)
                {
                    update_window_title(c, *win, cp1252_to_u16(read_string<char>(c.win_emu.memory, l_param)));
                }
                else
                {
                    update_window_title(c, *win, read_string<char16_t>(c.win_emu.memory, l_param));
                }

                // Deliver the text in the target proc's encoding (caller's is `ansi`, proc's is
                // unicode_proc): when they differ, re-encode into a scratch guest buffer so a wide proc
                // never reads ANSI as UTF-16. Freed in completion_NtUserMessageCall.
                const bool caller_unicode = ansi == FALSE;
                if (l_param != 0 && caller_unicode != win->unicode_proc)
                {
                    if (win->unicode_proc)
                    {
                        const auto wide = cp1252_to_u16(read_string<char>(c.win_emu.memory, l_param));
                        const auto bytes = (wide.size() + 1) * sizeof(char16_t);
                        scratch_text =
                            c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(bytes)), memory_permission::read_write);
                        c.win_emu.memory.write_memory(scratch_text, wide.c_str(), bytes);
                    }
                    else
                    {
                        const auto narrow = u16_to_cp1252(read_string<char16_t>(c.win_emu.memory, l_param));
                        const auto bytes = narrow.size() + 1;
                        scratch_text =
                            c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(bytes)), memory_permission::read_write);
                        c.win_emu.memory.write_memory(scratch_text, narrow.c_str(), bytes);
                    }
                    dispatch_l_param = scratch_text;
                }
            }

            message_call_state state{};
            state.window = hwnd;
            state.message = msg;
            state.scratch_text = scratch_text;
            dispatch_window_message(c, callback_id::NtUserMessageCall, std::move(state), *win, msg, w_param, dispatch_l_param);
            return {};
        }

        uint64_t completion_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t /*w_param*/,
                                              const uint64_t /*l_param*/, const uint64_t result_info, const DWORD type, const BOOL /*ansi*/)
        {
            auto& state = c.get_completion_state<message_call_state>();
            if (state.scratch_text != 0)
            {
                c.win_emu.memory.release_memory(state.scratch_text, 0);
            }
            if (state.message == WM_PAINT)
            {
                if (auto* win = c.proc.windows.get(state.window))
                {
                    (void)handle_NtGdiFlush(c);
                    present_existing_guest_window_surface(c, *win);
                    validate_window(*win);
                }
            }

            if (type == FNID_SENDMESSAGECALLBACK)
            {
                if (state.dispatching_result_callback)
                {
                    return TRUE;
                }

                if (result_info != 0)
                {
                    const auto* win = c.proc.windows.get(hwnd);
                    if (!win)
                    {
                        return TRUE;
                    }

                    send_message_callback_info callback_info{};
                    if (c.win_emu.memory.try_read_memory(result_info, &callback_info, sizeof(callback_info)) && callback_info.callback != 0)
                    {
                        fn_dword_message args{};
                        args.pwnd = win->guest.value();
                        args.msg = msg;
                        args.wParam = callback_info.data;
                        args.lParam = c.get_callback_result<uint64_t>();
                        args.xParam = callback_info.callback;
                        args.xpfnProc = c.proc.dispatch_client_message;

                        state.dispatching_result_callback = true;
                        dispatch_user_callback(c, callback_id::NtUserMessageCall, k_fn_dword_callback_id, std::move(state), args);
                        return {};
                    }
                }

                return TRUE;
            }

            return c.get_callback_result<uint64_t>();
        }

        uint64_t handle_NtUserDispatchMessage(const syscall_context& c, const emulator_object<msg> message)
        {
            if (!message)
            {
                return 0;
            }

            const auto m = message.read();
            auto* win = m.window != 0 ? c.proc.windows.get(m.window) : nullptr;
            if (m.window != 0 && !win)
            {
                return 0;
            }

            if (win && win->thread_id != c.vcpu.active_thread->id)
            {
                return 0;
            }

            if (m.message == WM_TIMER && m.lParam != 0)
            {
                set_thread_window_context(c, m.window, win ? win->guest.value() : 0);

                fn_dword_message args{};
                args.pwnd = win ? win->guest.value() : 0;
                args.msg = m.message;
                args.wParam = m.wParam;
                args.lParam = m.time;
                args.xParam = m.lParam;
                args.xpfnProc = c.proc.dispatch_client_message;

                message_call_state state{};
                state.window = m.window;
                state.message = m.message;
                dispatch_user_callback(c, callback_id::NtUserMessageCall, k_fn_dword_callback_id, args);
                return {};
            }

            if (!win)
            {
                return 0;
            }

            message_call_state state{};
            state.window = m.window;
            state.message = m.message;
            dispatch_window_message(c, callback_id::NtUserMessageCall, std::move(state), *win, m.message, m.wParam, m.lParam);
            return {};
        }

        BOOL handle_NtUserTranslateMessage(const syscall_context& /*c*/, const emulator_object<msg> /*message*/, const UINT /*flags*/)
        {
            return FALSE;
        }

        BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                     const UINT msg_filter_min, const UINT msg_filter_max)
        {
            auto& t = c.thread();

            if (auto pending_msg = t.peek_pending_message(c.win_emu, hwnd, msg_filter_min, msg_filter_max, true))
            {
                message.write(*pending_msg);
                t.current_message_time = pending_msg->time;
                set_thread_window_context(c, pending_msg->window);
                return pending_msg->message != WM_QUIT ? TRUE : FALSE;
            }

            t.await_msg = {message, hwnd, msg_filter_min, msg_filter_max};

            c.win_emu.yield_thread(c.vcpu, false);
            return {};
        }

        BOOL handle_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                      const UINT msg_filter_min, const UINT msg_filter_max, const UINT remove_message)
        {
            auto& t = c.thread();

            const bool should_remove = (remove_message & PM_REMOVE) != 0;
            std::optional<msg> pending_msg = t.peek_pending_message(c.win_emu, hwnd, msg_filter_min, msg_filter_max, should_remove);

            if (pending_msg)
            {
                message.write(*pending_msg);
                t.current_message_time = pending_msg->time;
                set_thread_window_context(c, pending_msg->window);
                return TRUE;
            }

            return FALSE;
        }

        BOOL handle_NtUserWaitMessage(const syscall_context& c)
        {
            auto& t = c.thread();
            if (t.peek_pending_message(c.win_emu))
            {
                return TRUE;
            }

            c.vcpu.active_thread->await_msg_mask = QS_ALLINPUT;
            c.win_emu.yield_thread(c.vcpu, false);
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

        void collect_pending_paint_tree(const syscall_context& c, window& win, std::vector<uint64_t>& order)
        {
            if (win.update_pending && win.thread_id == c.vcpu.active_thread->id)
            {
                order.push_back(static_cast<uint64_t>(win.handle));
            }

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle == win.handle && (child.style & WS_VISIBLE) != 0)
                {
                    collect_pending_paint_tree(c, child, order);
                }
            }
        }

        BOOL dispatch_pending_window_paint(const syscall_context& c, window_update_state state)
        {
            while (!state.pending.empty())
            {
                if (auto* win = c.proc.windows.get(static_cast<hwnd>(state.pending.back())))
                {
                    // Painting continues asynchronously through completion_NtUserUpdateWindow; the guest is
                    // suspended here, so this immediate return value is unused (matches handle_NtUserShowWindow).
                    dispatch_window_message(c, callback_id::NtUserUpdateWindow, std::move(state), *win, WM_PAINT);
                    return {};
                }
                state.pending.pop_back();
            }
            return TRUE;
        }

        BOOL handle_NtUserUpdateWindow(const syscall_context& c, const hwnd hwnd)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            // UpdateWindow paints synchronously, bypassing the message queue. Apps such as the open-iw5 splash
            // rely on this to display content without running a message loop, so a merely queued WM_PAINT would
            // never be pumped. Dispatch WM_PAINT now to the window and its invalid visible child controls.
            // Cross-thread windows cannot be painted on this thread, so fall back to posting the paint.
            if (win->thread_id != c.vcpu.active_thread->id)
            {
                if (win->update_pending)
                {
                    queue_window_paint(c, *win);
                }
                return TRUE;
            }

            std::vector<uint64_t> order;
            collect_pending_paint_tree(c, *win, order);
            if (order.empty())
            {
                return TRUE;
            }

            // back() is dispatched first, so reverse the parent-first paint order into the queue.
            window_update_state state{};
            state.pending.assign(order.rbegin(), order.rend());
            return dispatch_pending_window_paint(c, std::move(state));
        }

        BOOL completion_NtUserUpdateWindow(const syscall_context& c, const hwnd /*hwnd*/)
        {
            auto& state = c.get_completion_state<window_update_state>();

            if (!state.pending.empty())
            {
                const auto painted = state.pending.back();
                state.pending.pop_back();

                if (auto* win = c.proc.windows.get(static_cast<hwnd>(painted)))
                {
                    (void)handle_NtGdiFlush(c);
                    present_existing_guest_window_surface(c, *win);
                    validate_window(*win);
                }
            }

            return dispatch_pending_window_paint(c, std::move(state));
        }

        BOOL handle_NtUserPostMessage(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t wParam,
                                      const uint64_t lParam)
        {
            const auto* win = c.proc.windows.get(hwnd);
            if (!win && hwnd != 0)
            {
                return FALSE;
            }

            uint32_t target_thread_id = hwnd != 0 ? win->thread_id : c.thread().id;

            if (auto* thread = c.proc.find_thread_by_id(target_thread_id))
            {
                sogen::msg qmsg{};
                qmsg.window = hwnd;
                qmsg.message = msg;
                qmsg.wParam = wParam;
                qmsg.lParam = lParam;

                thread->post_message(c.win_emu, qmsg);
                return TRUE;
            }

            return FALSE;
        }

        BOOL handle_NtUserPostThreadMessage(const syscall_context& c, const DWORD id_thread, const UINT msg, const uint64_t wParam,
                                            const uint64_t lParam)
        {
            if (auto* thread = c.proc.find_thread_by_id(id_thread))
            {
                sogen::msg qmsg{};
                qmsg.message = msg;
                qmsg.wParam = wParam;
                qmsg.lParam = lParam;

                thread->post_message(c.win_emu, qmsg);
                return TRUE;
            }

            return FALSE;
        }

        BOOL handle_NtUserPostQuitMessage(const syscall_context& c, int exit_code)
        {
            sogen::msg qmsg{};
            qmsg.message = WM_QUIT;
            qmsg.wParam = exit_code;

            c.vcpu.active_thread->post_message(c.win_emu, qmsg);
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
            if (!dev_mode)
            {
                return STATUS_UNSUCCESSFUL;
            }

            const auto dev_name = device_name ? read_unicode_string(c.emu, device_name) : u"";

            if (!dev_name.empty() && dev_name != u"\\\\.\\DISPLAY1")
            {
                return STATUS_UNSUCCESSFUL;
            }

            // The single virtual display advertises a list of common modes so callers (e.g. DXVK's
            // mode enumeration, which the game uses to validate available resolutions) see real entries.
            struct display_mode
            {
                uint32_t width;
                uint32_t height;
            };
            static constexpr std::array<display_mode, 8> modes = {{
                {.width = 640, .height = 480},
                {.width = 800, .height = 600},
                {.width = 1024, .height = 768},
                {.width = 1280, .height = 720},
                {.width = 1280, .height = 1024},
                {.width = 1600, .height = 900},
                {.width = 1680, .height = 1050},
                {.width = 1920, .height = 1080},
            }};

            const auto fill_mode = [&](const uint32_t width, const uint32_t height) {
                dev_mode.access([&](EMU_DEVMODEW& dm) {
                    // Real EnumDisplaySettings fills the whole structure. Clear it first so fields the
                    // caller did not initialize (notably dmDisplayFlags, which holds DM_INTERLACED) do
                    // not leak stale stack data; DXVK rejects every mode whose dmDisplayFlags reports
                    // an interlaced mode.
                    dm = EMU_DEVMODEW{};
                    dm.dmSize = sizeof(EMU_DEVMODEW);
                    dm.dmFields = 0x5C0000; // DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY
                    dm.dmPelsWidth = width;
                    dm.dmPelsHeight = height;
                    dm.dmBitsPerPel = 32;
                    dm.dmDisplayFrequency = 60;
                });
            };

            if (mode_num == ENUM_CURRENT_SETTINGS || mode_num == ENUM_REGISTRY_SETTINGS)
            {
                fill_mode(1920, 1080);
                return STATUS_SUCCESS;
            }

            if (mode_num < modes.size())
            {
                fill_mode(modes[mode_num].width, modes[mode_num].height);
                return STATUS_SUCCESS;
            }

            return STATUS_UNSUCCESSFUL;
        }

        // The emulator owns a single virtual display whose mode never actually changes; accept any requested mode
        // (including CDS_TEST probes) by reporting DISP_CHANGE_SUCCESSFUL so the renderer proceeds to window setup.
        LONG handle_NtUserChangeDisplaySettings(const syscall_context& /*c*/,
                                                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*device_name*/,
                                                const emulator_object<EMU_DEVMODEW> /*dev_mode*/, const hwnd /*window*/,
                                                const DWORD /*flags*/, const uint64_t /*param*/)
        {
            return 0; // DISP_CHANGE_SUCCESSFUL
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

        BOOL handle_NtUserGetDpiForMonitor(const syscall_context& c, const handle monitor, const uint32_t dpi_type,
                                           const emulator_object<uint32_t> dpi_x, const emulator_object<uint32_t> dpi_y)
        {
            if (monitor != c.proc.default_monitor_handle || dpi_type > 2 || !dpi_x || !dpi_y)
            {
                return FALSE;
            }

            dpi_x.write(96);
            dpi_y.write(96);
            return TRUE;
        }

        BOOL completion_NtUserEnumDisplayMonitors(const syscall_context& c, const hdc /*hdc_in*/, const uint64_t /*clip_rect_ptr*/,
                                                  const uint64_t /*callback*/, const uint64_t /*param*/)
        {
            return c.get_callback_result<BOOL>();
        }

        BOOL handle_NtUserInheritWindowMonitor(const syscall_context& c, const hwnd hwnd_tgt, const hwnd hwnd_inherit)
        {
            return c.proc.windows.get(hwnd_tgt) != nullptr && (hwnd_inherit == 0 || c.proc.windows.get(hwnd_inherit) != nullptr);
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

        hwnd handle_NtUserSetParent(const syscall_context& c, const hwnd hwnd_child, const hwnd hwnd_new_parent)
        {
            auto* child = c.proc.windows.get(hwnd_child);
            if (!child)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return 0;
            }

            auto* new_parent = c.proc.windows.get(hwnd_new_parent);
            if (hwnd_new_parent != 0 && !new_parent)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return 0;
            }

            const hwnd desktop = c.proc.default_desktop_window_handle.bits;
            auto* effective_parent = new_parent;
            if (!effective_parent)
            {
                effective_parent = c.proc.windows.get(desktop);
            }

            hwnd old_parent = child->parent_handle;
            if (old_parent == desktop)
            {
                old_parent = 0;
            }

            if (!effective_parent)
            {
                child->parent_handle = 0;
                child->guest.access([&](USER_WINDOW& guest_win) {
                    guest_win.spwndParent = 0;
                    guest_win.spwndPrev = 0;
                    guest_win.spwndNext = 0;
                });
                return old_parent;
            }

            auto* ancestor = effective_parent;
            for (size_t guard = 0; ancestor != nullptr && guard < c.proc.windows.size(); ++guard)
            {
                if (ancestor->handle == child->handle)
                {
                    set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                    return 0;
                }

                ancestor = c.proc.windows.get(ancestor->parent_handle);
            }

            const auto child_ptr = child->guest.value();
            const auto unlink_from_parent = [&] {
                const auto guest_child = child->guest.read();
                if (guest_child.spwndParent != 0)
                {
                    emulator_object<USER_WINDOW>{c.emu, guest_child.spwndParent}.access([&](USER_WINDOW& parent_guest) {
                        if (parent_guest.spwndChild == child_ptr)
                        {
                            parent_guest.spwndChild = guest_child.spwndNext;
                        }
                    });
                }
                if (guest_child.spwndPrev != 0)
                {
                    emulator_object<USER_WINDOW>{c.emu, guest_child.spwndPrev}.access([&](USER_WINDOW& previous_guest) {
                        if (previous_guest.spwndNext == child_ptr)
                        {
                            previous_guest.spwndNext = guest_child.spwndNext;
                        }
                    });
                }
                if (guest_child.spwndNext != 0)
                {
                    emulator_object<USER_WINDOW>{c.emu, guest_child.spwndNext}.access([&](USER_WINDOW& next_guest) {
                        if (next_guest.spwndPrev == child_ptr)
                        {
                            next_guest.spwndPrev = guest_child.spwndPrev;
                        }
                    });
                }
            };

            const auto append_to_parent = [&] {
                uint64_t current = 0;
                effective_parent->guest.access([&](USER_WINDOW& parent_guest) {
                    current = parent_guest.spwndChild;
                    if (current == 0)
                    {
                        parent_guest.spwndChild = child_ptr;
                    }
                });

                for (size_t guard = 0; current != 0 && guard < c.proc.windows.size(); ++guard)
                {
                    uint64_t following = 0;
                    bool appended = false;
                    emulator_object<USER_WINDOW>{c.emu, current}.access([&](USER_WINDOW& current_guest) {
                        following = current_guest.spwndNext;
                        if (following == 0)
                        {
                            current_guest.spwndNext = child_ptr;
                            appended = true;
                        }
                    });
                    if (appended)
                    {
                        child->guest.access([&](USER_WINDOW& guest_child) { guest_child.spwndPrev = current; });
                        return;
                    }

                    current = following;
                }
            };

            unlink_from_parent();

            child->parent_handle = effective_parent->handle;
            child->guest.access([&](USER_WINDOW& guest_win) {
                guest_win.spwndParent = effective_parent->guest.value();
                guest_win.spwndPrev = 0;
                guest_win.spwndNext = 0;
            });

            append_to_parent();

            return old_parent;
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
                // Repaint the now-visible window and its child controls (see invalidate_window_tree).
                invalidate_window_tree(c, *win);
            }

            return TRUE;
        }

        NTSTATUS handle_NtUserSetForegroundWindow()
        {
            return STATUS_SUCCESS;
        }

        hwnd find_foreground_window(const syscall_context& c)
        {
            // Prefer the window the user last interacted with, if it still exists.
            if (c.proc.foreground_window != 0 && c.proc.windows.get(c.proc.foreground_window) != nullptr)
            {
                return c.proc.foreground_window;
            }

            // Otherwise fall back to any visible top-level window so a freshly-created game window is
            // considered foreground before the first mouse event arrives (games gate input on this).
            for (const auto& [index, win] : c.proc.windows)
            {
                if (win.parent_handle == 0 && (win.style & WS_VISIBLE) != 0)
                {
                    return win.handle;
                }
            }

            return 0;
        }

        hwnd handle_NtUserGetForegroundWindow(const syscall_context& c)
        {
            return find_foreground_window(c);
        }

        hwnd handle_NtUserSetFocus(const syscall_context& c, const hwnd hwnd)
        {
            if (hwnd != 0)
            {
                set_thread_window_context(c, hwnd);
            }
            return hwnd;
        }

        emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                      BOOL /*Ansi*/)
        {
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                return 0;
            }

            const auto entry = c.proc.classes.find(win->class_name);
            if (entry == c.proc.classes.end())
            {
                return 0;
            }

            auto& cls = entry->second.wnd_class;
            emulator_pointer old_value = 0;

            // GET/SET CLASS LONG indices (winuser.h)
            constexpr int gcl_style = -26;
            constexpr int gclp_wndproc = -24;
            constexpr int gclp_hmodule = -16;
            constexpr int gclp_hicon = -14;
            constexpr int gclp_hcursor = -12;
            constexpr int gclp_hbrbackground = -10;
            constexpr int gclp_hiconsm = -34;

            switch (nIndex)
            {
            case gcl_style:
                old_value = cls.style;
                cls.style = static_cast<uint32_t>(dwNewLong);
                break;
            case gclp_wndproc:
                old_value = cls.lpfnWndProc;
                cls.lpfnWndProc = dwNewLong;
                break;
            case gclp_hmodule:
                old_value = cls.hInstance;
                cls.hInstance = dwNewLong;
                break;
            case gclp_hicon:
                old_value = cls.hIcon;
                cls.hIcon = dwNewLong;
                break;
            case gclp_hcursor:
                old_value = cls.hCursor;
                cls.hCursor = dwNewLong;
                break;
            case gclp_hbrbackground:
                old_value = cls.hbrBackground;
                cls.hbrBackground = dwNewLong;
                break;
            case gclp_hiconsm:
                old_value = cls.hIconSm;
                cls.hIconSm = dwNewLong;
                break;
            default:
                // GCL_CBCLSEXTRA / GCL_CBWNDEXTRA / GCLP_MENUNAME / positive class-extra offsets: not modeled.
                break;
            }

            return old_value;
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
                        // Mirror to spmenu too (builds disagree on the id offset; see CreateWindowEx);
                        // only child windows store the id there, top-level keeps a real menu handle.
                        if ((guest_win.dwStyle & WS_CHILD) != 0)
                        {
                            guest_win.spmenu = dwNewLong;
                        }
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
                        win->wnd_proc = dwNewLong;
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

            uint64_t pExtraBytes = 0;
            win->guest.access([&](USER_WINDOW& guest_win) {
                pExtraBytes = guest_win.pExtraBytes;
                // WND+0x12 bit 0 marks "has DLGINFO". Without it user32 re-creates the DLGINFO on
                // every dialog message, so the modal loop and EndDialog touch different blocks and the
                // dialog never ends. The real kernel sets it when associating the pointer.
                if (ptr != 0)
                {
                    guest_win.bFlags |= 0x1;
                }
                else
                {
                    guest_win.bFlags &= static_cast<uint8_t>(~0x1);
                }
            });

            if (pExtraBytes != 0)
            {
                c.win_emu.memory.write_memory(pExtraBytes + sizeof(uint64_t), &ptr, sizeof(ptr));
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

            (void)ensure_system_menu(c, *win);
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

            invalidate_window(c, *win);
            return was_enabled ? FALSE : TRUE;
        }

        BOOL handle_NtUserDeleteMenu(const syscall_context& c, const uint64_t menu, const UINT position, const UINT flags)
        {
            return handle_NtUserRemoveMenu(c, static_cast<hmenu>(menu), position, flags);
        }

        uint64_t handle_NtUserGetSystemMenu(const syscall_context& c, const hwnd hwnd, const BOOL revert)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            if (revert != FALSE && win->system_menu_handle != 0)
            {
                if (auto* menu = c.proc.menus.get(win->system_menu_handle))
                {
                    menu->items.clear();
                    menu->sync_guest_items(c.win_emu.memory);
                }
            }

            return ensure_system_menu(c, *win);
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

        NTSTATUS handle_NtUserGetDisplayConfigBufferSizes(const syscall_context& c, const UINT32 /*flags*/,
                                                          const emulator_object<UINT32> num_path_array_elements,
                                                          const emulator_object<UINT32> num_mode_info_array_elements)
        {
            if (!num_path_array_elements || !num_mode_info_array_elements)
            {
                return STATUS_INVALID_PARAMETER;
            }

            // Use non-throwing writes: a failed guest write (e.g. a caller passing a bogus output pointer)
            // must surface as an error to the caller, not abort the whole emulator with an unhandled
            // host-side memory exception.
            const UINT32 path_count = 1;
            const UINT32 mode_count = 2;
            if (!c.win_emu.memory.try_write_memory(num_path_array_elements.value(), &path_count, sizeof(path_count)) ||
                !c.win_emu.memory.try_write_memory(num_mode_info_array_elements.value(), &mode_count, sizeof(mode_count)))
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context& c, const UINT32 /*flags*/,
                                                 const emulator_object<UINT32> num_path_array_elements, const emulator_pointer path_array,
                                                 const emulator_object<UINT32> current_topology_id, const emulator_pointer /*reserved*/)
        {
            if (!num_path_array_elements)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto num_paths = num_path_array_elements.read();

            num_path_array_elements.write(1);

            if (current_topology_id)
            {
                current_topology_id.write(0x1); // DISPLAYCONFIG_TOPOLOGY_INTERNAL
            }

            if (path_array && num_paths >= 1)
            {
                struct EMU_CCD_PATH_INFO
                {
                    UINT64 flags;
                    UINT64 padding1;
                    LUID adapterId;
                    UINT32 sourceId;
                    UINT32 targetId;
                    EMU_DISPLAYCONFIG_VIDEO_SIGNAL_INFO targetSignalInfo;
                    UINT32 outputTechnology;
                    UINT8 padding2[40]; // NOLINT
                    UINT32 sourceWidth;
                    UINT32 sourceHeight;
                    UINT8 padding3[84]; // NOLINT
                } internal_path{};

                internal_path.flags = 0x2000000000020003ULL;
                internal_path.adapterId = {.LowPart = 0x1000, .HighPart = 0};
                internal_path.sourceId = 0;
                internal_path.targetId = 0;
                internal_path.targetSignalInfo.pixelRate = 148500000;
                internal_path.targetSignalInfo.hSyncFreq = {.Numerator = 67500, .Denominator = 1};
                internal_path.targetSignalInfo.vSyncFreq = {.Numerator = 60, .Denominator = 1};
                internal_path.targetSignalInfo.activeSize = {.cx = 1920, .cy = 1080};
                internal_path.targetSignalInfo.totalSize = {.cx = 2200, .cy = 1125};
                internal_path.targetSignalInfo.scanLineOrdering = 1; // PROGRESSIVE
                internal_path.targetSignalInfo.u.videoStandard = 0;
                internal_path.outputTechnology = 5; // HDMI
                internal_path.padding2[17] = 1;
                internal_path.sourceWidth = 1920;
                internal_path.sourceHeight = 1080;

                c.emu.write_memory(path_array, &internal_path, sizeof(internal_path));
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUserDisplayConfigGetDeviceInfo(const syscall_context& c, const emulator_pointer packet)
        {
            if (!packet)
            {
                return STATUS_INVALID_PARAMETER;
            }

            auto header = c.emu.read_memory<EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER>(packet);

            switch (header.type)
            {
            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_SOURCE_NAME: {
                const emulator_object<EMU_DISPLAYCONFIG_SOURCE_DEVICE_NAME> source_name{c.emu, packet};

                source_name.access([&](EMU_DISPLAYCONFIG_SOURCE_DEVICE_NAME& sourceName) {
                    sourceName.header = header;
                    utils::string::copy(sourceName.viewGdiDeviceName, u"\\\\.\\DISPLAY1");
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_TARGET_NAME: {
                const emulator_object<EMU_DISPLAYCONFIG_TARGET_DEVICE_NAME> target_name{c.emu, packet};

                target_name.access([&](EMU_DISPLAYCONFIG_TARGET_DEVICE_NAME& targetName) {
                    targetName.header = header;
                    targetName.flags = 0x1;
                    targetName.outputTechnology = 5;
                    targetName.edidManufactureId = 0xABCD;
                    targetName.edidProductCodeId = 0x1234;
                    targetName.connectorInstance = 1;
                    utils::string::copy(targetName.monitorFriendlyDeviceName, u"Generic PnP Monitor");
                    utils::string::copy(targetName.monitorDevicePath,
                                        u"\\\\?\\DISPLAY#GSM0001#4&1234567&0&UID0#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_ADAPTER_NAME: {
                const emulator_object<EMU_DISPLAYCONFIG_ADAPTER_NAME> adapter_name{c.emu, packet};

                adapter_name.access([&](EMU_DISPLAYCONFIG_ADAPTER_NAME& adapterName) {
                    adapterName.header = header;
                    utils::string::copy(
                        adapterName.adapterDevicePath,
                        u"\\\\?\\PCI#VEN_10DE&DEV_1C03&SUBSYS_00000000&REV_A1#4&1234567&0&0008#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}");
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_ADVANCED_COLOR_INFO: {
                const emulator_object<EMU_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO> color_info{c.emu, packet};

                color_info.access([&](EMU_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO& colorInfo) {
                    colorInfo.header = header;
                    colorInfo.u.value = 0;
                    colorInfo.colorEncoding = 0;
                    colorInfo.bitsPerColorChannel = 8;
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_SOURCE_FROM_HASH: {
                const emulator_object<EMU_DISPLAYCONFIG_GET_SOURCE_FROM_HASH> source_from_hash{c.emu, packet};

                source_from_hash.access([](EMU_DISPLAYCONFIG_GET_SOURCE_FROM_HASH& req) {
                    req.adapterId = {.LowPart = 0x1000, .HighPart = 0}; // Must match k_dxgk_adapter_luid!
                    req.sourceId = 0;
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_DISPLAY_INFO: {
                if (header.size < sizeof(EMU_GET_DISPLAY_INFO))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const emulator_object<EMU_GET_DISPLAY_INFO> display_info{c.emu, packet};

                display_info.access([](EMU_GET_DISPLAY_INFO& info) {
                    const bool known_adapter =
                        info.adapterId.LowPart == 0x1000 && info.adapterId.HighPart == 0; // Must match k_dxgk_adapter_luid!

                    if (!known_adapter)
                    {
                        return;
                    }

                    const auto fill_block = [](EMU_DISPLAY_INFO_DEVICE_BLOCK& block) {
                        block.Valid = 1;
                        block.VendorID = 0x10DE;
                        block.DeviceID = 0x1C03;
                        block.SubSystemVendorID = 0x10DE;
                        block.SubSystemID = 0;
                        block.RevisionID = 0xA1;
                        block.WddmVersion = 3200;

                        utils::string::copy(block.AdapterDesc, u"NVIDIA GeForce GTX 1060 6GB");
                        utils::string::copy(
                            block.AdapterDevicePath,
                            u"\\\\?\\PCI#VEN_10DE&DEV_1C03&SUBSYS_00000000&REV_A1#4&1234567&0&0008#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}");
                    };

                    fill_block(info.DisplayAdapter);
                    fill_block(info.RenderAdapter);
                });

                return STATUS_SUCCESS;
            }

            case EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE::GET_DISPLAY_INFO_EX: {
                if (header.size < sizeof(EMU_GET_DISPLAY_INFO_EX))
                {
                    return STATUS_BUFFER_TOO_SMALL;
                }

                const emulator_object<EMU_GET_DISPLAY_INFO_EX> display_info{c.emu, packet};

                display_info.access([](EMU_GET_DISPLAY_INFO_EX& info) {
                    const bool known_adapter =
                        info.adapterId.LowPart == 0x1000 && info.adapterId.HighPart == 0; // Must match k_dxgk_adapter_luid!
                    const bool known_source = info.sourceId == 0;

                    info.FailurePoint = 0;

                    if (!known_adapter || !known_source)
                    {
                        return;
                    }

                    info.VendorID = 0x10DE;
                    info.DeviceID = 0x1C03;
                    info.SubSysID0 = 0;
                    info.SubSysID1 = 0;
                    info.RevisionID = 0xA1;
                    info.WddmVersion = 2700;

                    utils::string::copy(info.AdapterDesc, u"NVIDIA GeForce GTX 1060 6GB");

                    info.DisplayLeft = 0;
                    info.DisplayTop = 0;
                    info.DisplayWidth = 1920;
                    info.DisplayHeight = 1080;

                    utils::string::copy(info.DeviceName, u"\\\\.\\DISPLAY1");

                    info.ReservedQwordLow = 0;
                    info.ReservedQwordHigh = 0;
                });

                return STATUS_SUCCESS;
            }

            default:
                return STATUS_SUCCESS;
            }
        }

        uint64_t handle_NtUserInitThreadCoreMessagingIocp2(const syscall_context& c, const handle /*window_handle*/,
                                                           const emulator_object<uint32_t> completion_queue_index)
        {
            io_completion completion{};
            completion.number_of_concurrent_threads = 1;

            completion_queue_index.write_if_valid(0);
            return c.proc.io_completions.store(std::move(completion)).bits;
        }

        BOOL handle_NtUserDrainThreadCoreMessagingCompletions2()
        {
            return TRUE;
        }

        uint64_t handle_NtUserScheduleDispatchNotification(const syscall_context& /*c*/, const hwnd /*hwnd*/)
        {
            return 2;
        }

        uint64_t set_user_timer(const syscall_context& c, const hwnd hwnd, const uint64_t timer_id, const uint32_t elapsed_ms,
                                const uint64_t timer_proc, const bool is_system)
        {
            auto interval = std::chrono::milliseconds{elapsed_ms};
            if (interval < k_user_timer_minimum)
            {
                interval = k_user_timer_minimum;
            }

            auto* target_thread = c.vcpu.active_thread;

            if (hwnd != 0)
            {
                const auto* win = c.proc.windows.get(hwnd);
                if (!win)
                {
                    set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                    return 0;
                }

                target_thread = c.proc.find_thread_by_id(win->thread_id);

                if (!target_thread)
                {
                    set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                    return 0;
                }
            }

            const auto now = c.win_emu.clock().steady_now();

            if (auto* timer = target_thread->find_user_timer(hwnd, timer_id))
            {
                timer->interval = interval;
                timer->due_time = now + interval;
                timer->timer_proc = timer_proc;
                timer->is_system = is_system;
                return timer_id;
            }

            return target_thread->create_user_timer(c.proc, hwnd, timer_id, timer_proc, interval, now, is_system).timer_id;
        }

        uint64_t handle_NtUserSetTimer(const syscall_context& c, const hwnd hwnd, const uint64_t timer_id, const uint32_t elapsed_ms,
                                       const uint64_t timer_proc)
        {
            return set_user_timer(c, hwnd, timer_id, elapsed_ms, timer_proc, false);
        }

        uint64_t handle_NtUserSetSystemTimer(const syscall_context& c, const hwnd hwnd, const uint64_t timer_id, const uint32_t elapsed_ms)
        {
            return set_user_timer(c, hwnd, timer_id, elapsed_ms, 0, true);
        }

        BOOL handle_NtUserKillTimer(const syscall_context& c, const hwnd hwnd, const uint64_t timer_id)
        {
            auto* target_thread = c.vcpu.active_thread;

            if (hwnd != 0)
            {
                const auto* win = c.proc.windows.get(hwnd);
                if (!win)
                {
                    set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                    return FALSE;
                }

                target_thread = c.proc.find_thread_by_id(win->thread_id);

                if (!target_thread)
                {
                    set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                    return FALSE;
                }
            }

            if (!target_thread->delete_user_timer(hwnd, timer_id))
            {
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserValidateTimerCallback(const syscall_context& c, const uint64_t timer_proc)
        {
            if (timer_proc == 0)
            {
                return FALSE;
            }

            for (const auto& thread : c.proc.threads | std::views::values)
            {
                for (const auto& timer : thread.user_timers | std::views::values)
                {
                    if (timer.timer_proc == timer_proc)
                    {
                        return TRUE;
                    }
                }
            }

            return FALSE;
        }

        uint32_t handle_NtUserGetQueueStatusReadonly(const syscall_context& c, const UINT flags)
        {
            auto* thread = c.vcpu.active_thread;
            const auto current_bits = thread->get_message_queue_status(c.win_emu) & flags;
            const auto changed_bits = thread->queue_status_changed_bits & flags;
            return current_bits | (changed_bits << 16);
        }

        uint32_t handle_NtUserGetQueueStatus(const syscall_context& c, const UINT flags)
        {
            const auto result = handle_NtUserGetQueueStatusReadonly(c, flags);
            c.vcpu.active_thread->queue_status_changed_bits &= ~flags;
            return result;
        }

        uint64_t handle_NtUserCreateAcceleratorTable(const syscall_context& c, const emulator_pointer entries, const int32_t entry_count)
        {
            constexpr int32_t max_entry_count = 0x10000;
            if (entries == 0 || entry_count <= 0 || entry_count > max_entry_count)
            {
                return 0;
            }

            auto [table_handle, table] = c.proc.accelerator_tables.create(c.win_emu.memory);
            table.entries.resize(static_cast<size_t>(entry_count));
            if (!c.win_emu.memory.try_read_memory(entries, table.entries.data(), table.entries.size() * sizeof(accelerator_table_entry)))
            {
                c.proc.accelerator_tables.erase(table_handle);
                return 0;
            }

            return table_handle.bits;
        }

        BOOL handle_NtUserDestroyAcceleratorTable(const syscall_context& c, const handle accelerator_table)
        {
            return c.proc.accelerator_tables.erase(accelerator_table) ? TRUE : FALSE;
        }

        int32_t handle_NtUserCopyAcceleratorTable()
        {
            return 0;
        }

        int32_t handle_NtUserTranslateAccelerator()
        {
            return 0;
        }

        hmenu handle_NtUserCreateMenu(const syscall_context& c)
        {
            auto [handle, menu_obj] = c.proc.menus.create(c.win_emu.memory);
            menu_obj.handle = handle.bits;
            menu_obj.popup = false;
            menu_obj.init_guest();
            return handle.bits;
        }

        BOOL handle_NtUserThunkedMenuItemInfo(const syscall_context& c, const hmenu menu, const UINT position, const BOOL by_position,
                                              const BOOL insert, const emulator_object<EMU_MENUITEMINFO> item_info,
                                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> item_text)
        {
            auto* m = c.proc.menus.get(menu);
            if (!m)
            {
                return FALSE;
            }

            if (!item_info)
            {
                return FALSE;
            }

            EMU_MENUITEMINFO mi{};
            if (!c.win_emu.memory.try_read_memory(item_info.value(), &mi, sizeof(mi)))
            {
                return FALSE;
            }

            if (insert)
            {
                menu_item menu_item{};
                update_menu_item(c, menu_item, mi, item_text);

                if (by_position)
                {
                    const auto insert_at = std::min<size_t>(position, m->items.size());
                    m->items.insert(m->items.begin() + static_cast<ptrdiff_t>(insert_at), menu_item);
                }
                else
                {
                    const auto it = std::ranges::find_if(m->items, [&](const auto& item) { return item.id == position; });
                    m->items.insert(it, menu_item);
                }

                m->sync_guest_items(c.win_emu.memory);
                return TRUE;
            }

            if (m->items.empty())
            {
                return FALSE;
            }

            if (by_position)
            {
                if (position >= m->items.size())
                {
                    return FALSE;
                }

                const auto old_text_size = m->items[position].text.size();
                update_menu_item(c, m->items[position], mi, item_text);
                if ((mi.fMask & MIIM_STRING) != 0 && m->items[position].text.size() > old_text_size)
                {
                    m->sync_guest_items(c.win_emu.memory);
                }
                else
                {
                    m->sync_guest_item(c.win_emu.memory, position);
                }
                return TRUE;
            }

            const auto it = std::ranges::find_if(m->items, [&](const auto& item) { return item.id == position; });
            if (it == m->items.end())
            {
                return TRUE;
            }

            const auto index = static_cast<size_t>(std::distance(m->items.begin(), it));
            const auto old_text_size = it->text.size();
            update_menu_item(c, *it, mi, item_text);
            if ((mi.fMask & MIIM_STRING) != 0 && it->text.size() > old_text_size)
            {
                m->sync_guest_items(c.win_emu.memory);
            }
            else
            {
                m->sync_guest_item(c.win_emu.memory, index);
            }
            return TRUE;
        }

        hmenu handle_NtUserCreatePopupMenu(const syscall_context& c)
        {
            auto [handle, menu_obj] = c.proc.menus.create(c.win_emu.memory);
            menu_obj.handle = handle.bits;
            menu_obj.popup = true;
            menu_obj.init_guest();
            return handle.bits;
        }

        BOOL handle_NtUserSetMenu()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetMenuDefaultItem(const syscall_context& /*c*/, const hmenu /*menu*/, const UINT /*item*/,
                                             const UINT /*by_position*/)
        {
            return TRUE;
        }

        BOOL handle_NtUserEndMenu()
        {
            return TRUE;
        }

        BOOL handle_NtUserRemoveMenu(const syscall_context& c, const hmenu menu, const UINT position, const UINT flags)
        {
            auto* m = c.proc.menus.get(menu);
            if (!m || m->items.empty())
            {
                return FALSE;
            }

            if ((flags & MF_BYPOSITION) != 0)
            {
                if (position >= m->items.size())
                {
                    return FALSE;
                }

                m->items.erase(m->items.begin() + static_cast<ptrdiff_t>(position));
            }
            else
            {
                const auto item = std::ranges::find_if(m->items, [&](const auto& entry) { return entry.id == position; });
                if (item == m->items.end())
                {
                    return FALSE;
                }

                m->items.erase(item);
            }

            m->sync_guest_items(c.win_emu.memory);
            return TRUE;
        }

        BOOL handle_NtUserDestroyMenu(const syscall_context& c, const hmenu menu)
        {
            auto* m = c.proc.menus.get(menu);
            if (!m)
            {
                return FALSE;
            }

            m->release_guest_backing(c.win_emu.memory);
            return c.proc.menus.erase(menu) ? TRUE : FALSE;
        }

        BOOL handle_NtUserDrawMenuBar(const syscall_context& c, const hwnd hwnd)
        {
            if (hwnd != 0 && !c.proc.windows.get(hwnd))
            {
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserSetWindowCompositionAttribute(const syscall_context& c, const hwnd hwnd, const emulator_pointer /*data*/)
        {
            if (hwnd != 0 && !c.proc.windows.get(hwnd))
            {
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserCreateCaret()
        {
            return TRUE;
        }
        BOOL handle_NtUserDestroyCaret()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetCaretPos()
        {
            return TRUE;
        }

        BOOL handle_NtUserShowCaret()
        {
            return TRUE;
        }

        BOOL handle_NtUserHideCaret()
        {
            return TRUE;
        }

        BOOL handle_NtUserGetObjectInformation()
        {
            return FALSE;
        }

        uint64_t handle_NtUserQueryWindow(const syscall_context& c, const hwnd window_handle, const uint32_t query_type)
        {
            const auto* win = c.proc.windows.get(window_handle);
            if (!win)
            {
                return 0;
            }

            // WINDOWINFOCLASS: WindowProcess (0) returns the owning process id. Everything else resolves to
            // the thread.
            if (query_type == 0)
            {
                return process_context::process_id;
            }

            return win->thread_id;
        }

        int handle_NtUserSetScrollInfo()
        {
            return 0;
        }

        BOOL handle_NtUserIsTouchWindow()
        {
            return FALSE;
        }

        BOOL handle_NtUserGetWindowPlacement()
        {
            return TRUE;
        }

        BOOL handle_NtUserTrackMouseEvent()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetWindowRgn()
        {
            return TRUE;
        }

        BOOL handle_NtUserAlterWindowStyle()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetActiveWindow()
        {
            return TRUE;
        }

        NTSTATUS handle_NtUserSelectPalette()
        {
            return STATUS_SUCCESS;
        }

        BOOL handle_NtUserSwapMouseButton()
        {
            return TRUE;
        }

        int32_t handle_NtUserGetKeyNameText(const syscall_context& c, const int32_t l_param, const emulator_pointer buffer,
                                            const int32_t character_count)
        {
            const auto copy_u16_string_to_buffer = [](const syscall_context& c, const std::u16string_view text,
                                                      const emulator_pointer buffer, const int32_t character_count) -> uint64_t {
                if (character_count <= 0 || buffer == 0)
                {
                    return 0;
                }

                const auto capacity = static_cast<uint64_t>(character_count);
                const auto copy_count = std::min<uint64_t>(text.size(), capacity - 1);
                if (copy_count != 0 &&
                    !c.win_emu.memory.try_write_memory(buffer, text.data(), static_cast<size_t>(copy_count * sizeof(char16_t))))
                {
                    return 0;
                }

                const char16_t terminator = u'\0';
                if (!c.win_emu.memory.try_write_memory(buffer + copy_count * sizeof(char16_t), &terminator, sizeof(terminator)))
                {
                    return 0;
                }

                return copy_count;
            };

            const auto get_key_name_from_lparam = [](const uint64_t l_param) -> std::u16string {
                const auto scan_code = static_cast<uint32_t>((l_param >> 16) & 0xFFu);
                const auto extended = ((l_param >> 24) & 0x1u) != 0;

                if (scan_code >= 0x10 && scan_code <= 0x19)
                {
                    static constexpr std::u16string_view top_row = u"QWERTYUIOP";
                    return {1, top_row[scan_code - 0x10]};
                }

                if (scan_code >= 0x1E && scan_code <= 0x26)
                {
                    static constexpr std::u16string_view home_row = u"ASDFGHJKL";
                    return {1, home_row[scan_code - 0x1E]};
                }

                if (scan_code >= 0x2C && scan_code <= 0x32)
                {
                    static constexpr std::u16string_view bottom_row = u"ZXCVBNM";
                    return {1, bottom_row[scan_code - 0x2C]};
                }

                if (scan_code >= 0x02 && scan_code <= 0x0B)
                {
                    static constexpr std::u16string_view digits = u"1234567890";
                    return {1, digits[scan_code - 0x02]};
                }

                if (scan_code >= 0x3B && scan_code <= 0x44)
                {
                    const auto fn = 1u + (scan_code - 0x3B);
                    std::u16string name = u"F";
                    for (const char ch : std::to_string(fn))
                    {
                        name.push_back(static_cast<char16_t>(ch));
                    }
                    return name;
                }

                if (scan_code == 0x57)
                {
                    return u"F11";
                }

                if (scan_code == 0x58)
                {
                    return u"F12";
                }

                switch (scan_code)
                {
                case 0x01:
                    return u"Esc";
                case 0x0C:
                    return u"-";
                case 0x0D:
                    return u"=";
                case 0x0E:
                    return u"Backspace";
                case 0x0F:
                    return u"Tab";
                case 0x1A:
                    return u"[";
                case 0x1B:
                    return u"]";
                case 0x1C:
                    return u"Enter";
                case 0x1D:
                    return u"Ctrl";
                case 0x27:
                    return u";";
                case 0x28:
                    return u"'";
                case 0x29:
                    return u"`";
                case 0x2A:
                case 0x36:
                    return u"Shift";
                case 0x2B:
                    return u"\\";
                case 0x33:
                    return u",";
                case 0x34:
                    return u".";
                case 0x35:
                    return extended ? u"Num /" : u"/";
                case 0x37:
                    return extended ? u"PrtScn" : u"Num *";
                case 0x38:
                    return u"Alt";
                case 0x39:
                    return u"Space";
                case 0x3A:
                    return u"Caps Lock";
                case 0x45:
                    return extended ? u"Pause" : u"Num Lock";
                case 0x46:
                    return u"Scroll Lock";
                case 0x47:
                    return extended ? u"Home" : u"Num 7";
                case 0x48:
                    return extended ? u"Up" : u"Num 8";
                case 0x49:
                    return extended ? u"Page Up" : u"Num 9";
                case 0x4A:
                    return u"Num -";
                case 0x4B:
                    return extended ? u"Left" : u"Num 4";
                case 0x4C:
                    return u"Num 5";
                case 0x4D:
                    return extended ? u"Right" : u"Num 6";
                case 0x4E:
                    return u"Num +";
                case 0x4F:
                    return extended ? u"End" : u"Num 1";
                case 0x50:
                    return extended ? u"Down" : u"Num 2";
                case 0x51:
                    return extended ? u"Page Down" : u"Num 3";
                case 0x52:
                    return extended ? u"Insert" : u"Num 0";
                case 0x53:
                    return extended ? u"Delete" : u"Num Del";
                case 0x5B:
                    return u"Left Windows";
                case 0x5C:
                    return u"Right Windows";
                case 0x5D:
                    return u"Application";
                default:
                    return {};
                }
            };

            const auto name = get_key_name_from_lparam(static_cast<uint32_t>(l_param));
            if (name.empty())
            {
                return 0;
            }

            return static_cast<int32_t>(copy_u16_string_to_buffer(c, name, buffer, character_count));
        }

        hwnd handle_NtUserWindowFromPoint(const syscall_context& c, const int32_t /*x*/, const int32_t /*y*/)
        {
            // TODO: Properly resolve the topmost visible/enabled window containing the
            //       screen point, walking child windows in z-order; this stub just returns the
            //       current foreground window.
            return c.proc.foreground_window;
        }

        BOOL handle_NtUserGetKeyboardState(const syscall_context& c, const emulator_pointer key_state)
        {
            if (key_state == 0)
            {
                return FALSE;
            }

            if (!c.win_emu.memory.try_write_memory(key_state, c.proc.key_state.data(), c.proc.key_state.size()))
            {
                return FALSE;
            }

            return TRUE;
        }

        uint32_t handle_NtUserGetDoubleClickTime()
        {
            return 500;
        }

        BOOL handle_NtUserModifyWindowTouchCapability()
        {
            return TRUE;
        }

        uint32_t handle_NtUserGetClipboardSequenceNumber()
        {
            return 1;
        }

        BOOL handle_NtUserOpenClipboard()
        {
            return TRUE;
        }

        BOOL handle_NtUserCloseClipboard()
        {
            return TRUE;
        }

        BOOL handle_NtUserEmptyClipboard()
        {
            return TRUE;
        }

        uint64_t handle_NtUserGetClipboardData()
        {
            return 0;
        }

        uint64_t handle_NtUserConvertMemHandle()
        {
            // Return a non-null placeholder clipboard handle so SetClipboardData succeeds. The contents are
            // not stored; reads via NtUserGetClipboardData report an empty clipboard.
            return 1;
        }

        uint64_t handle_NtUserSetClipboardData()
        {
            return 1;
        }

        uint64_t handle_NtUserGetProcessDpiAwarenessContext()
        {
            return 0;
        }

        NTSTATUS handle_NtUserSetProcessDpiAwarenessContext()
        {
            return 0;
        }

        uint32_t handle_NtUserMapVirtualKeyEx(const syscall_context& /*c*/, const uint32_t code, const uint32_t map_type,
                                              const uint32_t /*keyboard_id*/, const uint64_t /*keyboard_layout*/)
        {
            constexpr std::array<uint8_t, 26> letter_scans{
                0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
            };
            constexpr std::array<uint8_t, 10> digit_scans{0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
            constexpr std::array<uint8_t, 10> numpad_scans{0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};

            const auto virtual_key_to_scan_code = [&](const uint32_t virtual_key) -> uint32_t {
                if (virtual_key >= 'A' && virtual_key <= 'Z')
                {
                    return letter_scans[virtual_key - 'A'];
                }
                if (virtual_key >= '0' && virtual_key <= '9')
                {
                    return digit_scans[virtual_key - '0'];
                }
                if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9)
                {
                    return numpad_scans[virtual_key - VK_NUMPAD0];
                }
                if (virtual_key >= VK_F1 && virtual_key <= VK_F10)
                {
                    return 0x3B + virtual_key - VK_F1;
                }

                switch (virtual_key)
                {
                case VK_F11:
                    return 0x57;
                case VK_F12:
                    return 0x58;
                case VK_ESCAPE:
                    return 0x01;
                case VK_BACK:
                    return 0x0E;
                case VK_TAB:
                    return 0x0F;
                case VK_RETURN:
                    return 0x1C;
                case VK_SHIFT:
                case 0xA0:
                    return 0x2A;
                case 0xA1:
                    return 0x36;
                case VK_CONTROL:
                case 0xA2:
                    return 0x1D;
                case 0xA3:
                    return 0xE01D;
                case VK_MENU:
                case 0xA4:
                    return 0x38;
                case 0xA5:
                    return 0xE038;
                case VK_PAUSE:
                    return 0xE11D;
                case VK_CAPITAL:
                    return 0x3A;
                case VK_SPACE:
                    return 0x39;
                case VK_PRIOR:
                    return 0xE049;
                case VK_NEXT:
                    return 0xE051;
                case VK_END:
                    return 0xE04F;
                case VK_HOME:
                    return 0xE047;
                case VK_LEFT:
                    return 0xE04B;
                case VK_UP:
                    return 0xE048;
                case VK_RIGHT:
                    return 0xE04D;
                case VK_DOWN:
                    return 0xE050;
                case VK_INSERT:
                    return 0xE052;
                case VK_DELETE:
                    return 0xE053;
                case VK_LWIN:
                    return 0xE05B;
                case VK_RWIN:
                    return 0xE05C;
                case VK_APPS:
                    return 0xE05D;
                case VK_MULTIPLY:
                    return 0x37;
                case VK_ADD:
                    return 0x4E;
                case VK_SUBTRACT:
                    return 0x4A;
                case VK_DECIMAL:
                    return 0x53;
                case VK_DIVIDE:
                    return 0xE035;
                case VK_NUMLOCK:
                    return 0x45;
                case VK_SCROLL:
                    return 0x46;
                case VK_OEM_1:
                    return 0x27;
                case VK_OEM_PLUS:
                    return 0x0D;
                case VK_OEM_COMMA:
                    return 0x33;
                case VK_OEM_MINUS:
                    return 0x0C;
                case VK_OEM_PERIOD:
                    return 0x34;
                case VK_OEM_2:
                    return 0x35;
                case VK_OEM_3:
                    return 0x29;
                case VK_OEM_4:
                    return 0x1A;
                case VK_OEM_5:
                    return 0x2B;
                case VK_OEM_6:
                    return 0x1B;
                case VK_OEM_7:
                    return 0x28;
                case VK_OEM_102:
                    return 0x56;
                default:
                    return 0;
                }
            };

            if (map_type == 0 || map_type == 4)
            {
                const auto scan_code = virtual_key_to_scan_code(code);
                return map_type == 0 ? scan_code & 0xFF : scan_code;
            }

            if (map_type == 2)
            {
                if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == VK_SPACE)
                {
                    return code;
                }

                switch (code)
                {
                case VK_OEM_1:
                    return ';';
                case VK_OEM_PLUS:
                    return '=';
                case VK_OEM_COMMA:
                    return ',';
                case VK_OEM_MINUS:
                    return '-';
                case VK_OEM_PERIOD:
                    return '.';
                case VK_OEM_2:
                    return '/';
                case VK_OEM_3:
                    return '`';
                case VK_OEM_4:
                    return '[';
                case VK_OEM_5:
                    return '\\';
                case VK_OEM_6:
                    return ']';
                case VK_OEM_7:
                    return '\'';
                default:
                    return 0;
                }
            }

            if (map_type == 1 || map_type == 3)
            {
                const auto scan_code = map_type == 1 ? code & 0xFF : code & 0xFFFF;
                if (map_type == 1)
                {
                    if (scan_code == 0x2A || scan_code == 0x36)
                    {
                        return VK_SHIFT;
                    }
                    if (scan_code == 0x1D)
                    {
                        return VK_CONTROL;
                    }
                    if (scan_code == 0x38)
                    {
                        return VK_MENU;
                    }
                }
                else
                {
                    switch (scan_code)
                    {
                    case 0x2A:
                        return 0xA0;
                    case 0x36:
                        return 0xA1;
                    case 0x1D:
                        return 0xA2;
                    case 0xE01D:
                        return 0xA3;
                    case 0x38:
                        return 0xA4;
                    case 0xE038:
                        return 0xA5;
                    default:
                        break;
                    }
                }

                for (uint32_t virtual_key = 1; virtual_key < 0x100; ++virtual_key)
                {
                    const auto candidate = virtual_key_to_scan_code(virtual_key);
                    if ((map_type == 1 ? candidate & 0xFF : candidate) == scan_code)
                    {
                        return virtual_key;
                    }
                }
            }

            return 0;
        }

        NTSTATUS handle_NtUserToUnicodeEx()
        {
            return 0;
        }

        uint64_t handle_NtUserSetKeyboardState()
        {
            return 0;
        }

        uint64_t handle_NtUserAttachThreadInput()
        {
            return 0;
        }

        BOOL handle_NtUserRegisterTouchHitTestingWindow()
        {
            return TRUE;
        }

        uint64_t handle_NtUserActivateKeyboardLayout()
        {
            return 0x1337;
        }
    }

} // namespace sogen
