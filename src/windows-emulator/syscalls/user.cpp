#include "../std_include.hpp"
#include "../emulated_display_adapter.hpp"
#include "../emulator_utils.hpp"
#include "../keyboard_layout.hpp"
#include "../low_level_hooks.hpp"
#include "../syscall_utils.hpp"
#include "../win32k_userconnect.hpp"
#include "../window_destroy_orchestrator.hpp"
#include "../window_hooks.hpp"
#include "../window_show_orchestrator.hpp"
#include "windows-emulator/user_callback_dispatch.hpp"
#include <limits>
#include <unordered_set>

#ifdef msg
#undef msg
#endif

namespace sogen
{

    namespace
    {
        constexpr ULONG k_thread_state_capture_window = 0x2;
        constexpr ULONG k_thread_state_send_message = 0x8;
        constexpr ULONG k_thread_state_win32_thread_info = 0xE;
        constexpr ULONG k_thread_state_dialog_state = 0xA;
        constexpr ULONG k_thread_state_message_time = 0x9;
        constexpr uint32_t k_ismex_send = 0x1;
        constexpr uint32_t k_ismex_replied = 0x8;
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
        constexpr uint32_t k_fn_hk_in_lp_cbt_create_struct_callback_id = 0x2A;
        constexpr DWORD k_error_invalid_hook_handle = 1404;
        constexpr DWORD k_error_invalid_hook_filter = 1426;
        constexpr DWORD k_error_invalid_filter_proc = 1427;
        constexpr DWORD k_error_hook_needs_hmod = 1428;
        constexpr DWORD k_error_global_only_hook = 1429;
        constexpr DWORD k_error_access_denied = 5;
        constexpr DWORD k_error_hook_not_installed = 1431;
        constexpr DWORD k_error_not_enough_memory = 8;
        constexpr uint64_t k_hook_index_mask = 0xFFFF;
        constexpr uint64_t k_hook_uniqueness_mask = 0x7FFF;
        constexpr uint32_t k_hook_uniqueness_last = 0x7FFE;
        constexpr size_t k_max_hook_text_length = 0x8000;
        constexpr int k_hcbt_createwnd = 3;

        constexpr size_t k_client_pfn_button_wndproc_index = 7;
        constexpr size_t k_client_pfn_dialog_wndproc_index = 10;
        constexpr size_t k_client_pfn_edit_wndproc_index = 11;
        constexpr size_t k_client_pfn_static_wndproc_index = 14;
        constexpr uint32_t k_ctlcolor_edit = 1;
        constexpr uint32_t k_ctlcolor_listbox = 2;
        constexpr uint32_t k_ctlcolor_scrollbar = 5;
        constexpr uint32_t k_color_scrollbar = 0;
        constexpr uint32_t k_color_window = 5;
        constexpr uint32_t k_color_btnface = 15;
        constexpr auto k_user_timer_minimum = std::chrono::milliseconds{10};
        constexpr uint64_t k_hrgn_window = 1;
        constexpr UINT k_cb_getcomboboxinfo = 0x0164;
        constexpr uint16_t k_combo_lbox_system_atom = 0x8012;
        constexpr uint32_t k_cbs_simple = 0x0001;
        constexpr uint32_t k_cbs_type_mask = 0x0003;
        constexpr uint32_t k_state_system_pressed = 0x00000008;
        constexpr uint32_t k_state_system_invisible = 0x00008000;
        constexpr size_t k_sm_cxvscroll_index = 2;

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

        struct fn_hk_in_lp_cbt_create_struct_message
        {
            user_callback_capture_buffer captureBuffer{};
            pointer pwnd{};
            UINT msg{};
            wparam wParam{};
            EMU_CREATESTRUCT cs{};
            hwnd hwndInsertAfter{};
            pointer xpfnProc{};
            BOOL ansi{};
        };

        static_assert(sizeof(fn_hk_in_lp_cbt_create_struct_message) == 0xA8);

        static_assert(offsetof(fn_hk_in_lp_cbt_create_struct_message, cs) == 0x40);
        static_assert(offsetof(fn_hk_in_lp_cbt_create_struct_message, hwndInsertAfter) == 0x90);
        static_assert(offsetof(fn_hk_in_lp_cbt_create_struct_message, xpfnProc) == 0x98);
        static_assert(offsetof(fn_hk_in_lp_cbt_create_struct_message, ansi) == 0xA0);

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
                EMU_WINDOWPOS32 window_pos32;
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

        // The interactive station, which the process starts on and which is not held in the store.
        constexpr uint64_t interactive_window_station = 1;

        std::u16string make_atom_class_name(const uint16_t atom)
        {
            std::u16string name = u"#";
            for (const char ch : std::to_string(atom))
            {
                name.push_back(static_cast<char16_t>(ch));
            }

            return name;
        }

        // user32 answers GetClassLongPtr from the class object, never through a syscall, so it has to carry the
        // registered values.
        void publish_class_to_guest(memory_manager& memory, const process_context::class_entry& entry)
        {
            emulator_object<USER_CLASS>{memory, entry.guest_obj_addr}.access([&](USER_CLASS& cls) {
                cls.atom = entry.atom;
                cls.style = entry.wnd_class.style;
                cls.cbclsExtra = static_cast<uint32_t>(entry.wnd_class.cbClsExtra);
                cls.lpszClientAnsiMenuName = entry.menu_name.pszClientAnsiMenuName;
                cls.lpszClientUnicodeMenuName = entry.menu_name.pwszClientUnicodeMenuName;
                cls.lpfnWndProc = entry.wnd_class.lpfnWndProc;
                cls.hIconSm = entry.wnd_class.hIconSm;
                cls.hModule = entry.wnd_class.hInstance;
                cls.hbrBackground = entry.wnd_class.hbrBackground;
                cls.cbwndExtra = static_cast<uint32_t>(entry.wnd_class.cbWndExtra);
            });
        }

        bool is_builtin_window_class_name(const std::u16string_view class_name)
        {
            const auto normalized = normalize_builtin_window_class_name(class_name);
            return normalized == builtin_dialog_class_name || normalized == u"Button" || normalized == u"Edit" || normalized == u"Static";
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
            if (normalized == u"Edit")
            {
                return 0x02A5;
            }
            if (normalized == u"Static")
            {
                return 0x02A8;
            }

            return 0;
        }

        uint16_t builtin_window_class_atom(process_context& proc, const std::u16string_view normalized_name)
        {
            if (normalized_name == builtin_dialog_class_name)
            {
                return builtin_dialog_class_atom;
            }

            return proc.add_or_find_atom(std::u16string{normalized_name});
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
                else if (normalized_name == u"Edit")
                {
                    wnd_proc = server_info.apfnClientW[k_client_pfn_edit_wndproc_index];
                    if (wnd_proc == 0)
                    {
                        wnd_proc = server_info.apfnClientA[k_client_pfn_edit_wndproc_index];
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

            if (normalized_name == u"Button" || normalized_name == u"Edit" || normalized_name == u"Static")
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

            const auto cls_ptr = process_context::allocate_user_class(c.win_emu.memory, normalized_name);

            EMU_WNDCLASSEX wnd_class{};
            wnd_class.cbSize = sizeof(wnd_class);
            wnd_class.lpfnWndProc = wnd_proc;
            wnd_class.cbWndExtra = wnd_extra;

            auto entry = process_context::class_entry{
                cls_ptr, wnd_class, {}, builtin_window_class_atom(c.proc, normalized_name), std::u16string{normalized_name}};
            entry.synthesised = true;
            publish_class_to_guest(c.win_emu.memory, entry);
            emulator_object<USER_CLASS>{c.win_emu.memory, cls_ptr}.access(
                [&](USER_CLASS& cls) { cls.fnid = get_builtin_window_fnid(normalized_name); });
            c.proc.classes.insert_or_assign(std::u16string{normalized_name}, entry);
            c.proc.classes.insert_or_assign(std::u16string{class_name}, entry);
            return &c.proc.classes.find(class_name)->second;
        }

        void set_thread_window_context(const syscall_context& c, const uint64_t active_handle, const uint64_t active_window_ptr)
        {
            if (c.vcpu.active_thread && c.vcpu.active_thread->teb64)
            {
                set_cached_window(*c.vcpu.active_thread->teb64, active_handle, active_window_ptr);
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

        bool is_application_active(const syscall_context& c)
        {
            const auto* foreground = c.proc.windows.get(c.proc.foreground_window);
            return foreground != nullptr && !foreground->message_only;
        }

        void set_user_handle_owner(const syscall_context& c, const handle h, const uint64_t owner)
        {
            if (owner == 0)
            {
                return;
            }

            const auto index = static_cast<uint32_t>(h.value.id);
            if (index == 0 || index >= user_handle_table::MAX_HANDLE_INDICES)
            {
                return;
            }

            c.proc.user_handles.set_owner(index, owner);
        }

        enum class child_redraw_scope
        {
            none,
            unclipped,
            all,
        };

        bool reaches_children(const window& parent, const child_redraw_scope scope)
        {
            return scope == child_redraw_scope::all || (scope == child_redraw_scope::unclipped && (parent.style & WS_CLIPCHILDREN) == 0);
        }

        // The part of a parent's client area that falls into the child's client area, in the child's coordinates.
        std::optional<RECT> area_in_child(const window& child, const RECT& parent_area)
        {
            const RECT child_area{
                .left = std::max<LONG>(parent_area.left - child.client_x(), 0),
                .top = std::max<LONG>(parent_area.top - child.client_y(), 0),
                .right = std::min<LONG>(parent_area.right - child.client_x(), child.client_width()),
                .bottom = std::min<LONG>(parent_area.bottom - child.client_y(), child.client_height()),
            };
            if (child_area.left >= child_area.right || child_area.top >= child_area.bottom)
            {
                return std::nullopt;
            }
            return child_area;
        }

        void invalidate_window(const syscall_context& c, window& win, const std::optional<RECT>& update_rect, bool erase,
                               child_redraw_scope scope = child_redraw_scope::unclipped);

        RECT get_client_rect(const window& win)
        {
            return RECT{.left = 0, .top = 0, .right = win.client_width(), .bottom = win.client_height()};
        }

        RECT get_screen_window_rect(const syscall_context& c, const window& win)
        {
            const auto origin = c.proc.window_screen_origin(win);
            return RECT{.left = origin.x, .top = origin.y, .right = origin.x + win.width, .bottom = origin.y + win.height};
        }

        RECT get_window_rect(const window& win)
        {
            return RECT{.left = win.x, .top = win.y, .right = win.x + win.width, .bottom = win.y + win.height};
        }

        uint32_t filter_supported_window_style_change(const window& win, const int index, const uint32_t new_style)
        {
            if (index == GWL_EXSTYLE)
            {
                constexpr uint32_t frame_mask =
                    WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOOLWINDOW | WS_EX_CONTEXTHELP;
                return (new_style & ~frame_mask) | (win.ex_style & frame_mask);
            }

            uint32_t preserved_mask = WS_CHILD | WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_HSCROLL | WS_VSCROLL;
            if ((win.style & WS_CHILD) == 0)
            {
                // Child controls use these bits for WS_GROUP and WS_TABSTOP.
                preserved_mask |= WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            }

            // TODO: Support frame refresh and child/top-level transitions before allowing these bits to change.
            return (new_style & ~preserved_mask) | (win.style & preserved_mask);
        }

        ui_insets get_host_ui_client_insets(const window& win)
        {
            const auto insets = win.nonclient_insets();
            return {.left = insets.left, .top = insets.top, .right = insets.right, .bottom = insets.bottom};
        }

        void update_window_geometry(const syscall_context& c, window& win, const int x, const int y, const int width, const int height,
                                    const bool repaint)
        {
            const bool geometry_changed = win.x != x || win.y != y || win.width != width || win.height != height;
            win.x = x;
            win.y = y;
            win.width = width;
            win.height = height;
            c.proc.sync_guest_window_rects(win);

            if (win.host_surface_window)
            {
                c.win_emu.ui().set_window_rect(win.handle, get_window_rect(win));
            }

            // Native SetWindowPos and MoveWindow do not create an update region when the requested geometry is unchanged.
            if (repaint && geometry_changed)
            {
                invalidate_window(c, win, std::nullopt, false);
            }
        }

        std::optional<EMU_WINDOWPOS> read_window_pos_callback_output(const syscall_context& c)
        {
            const auto& result = c.get_callback_result_object();
            if (result.output == 0)
            {
                return std::nullopt;
            }

            if (c.proc.is_wow64_process)
            {
                if (result.output_size < sizeof(EMU_WINDOWPOS32))
                {
                    return std::nullopt;
                }

                EMU_WINDOWPOS32 position{};
                if (!c.win_emu.memory.try_read_memory(result.output, &position, sizeof(position)))
                {
                    return std::nullopt;
                }

                return EMU_WINDOWPOS{
                    .hwnd = position.hwnd,
                    .hwndInsertAfter = position.hwndInsertAfter,
                    .x = position.x,
                    .y = position.y,
                    .cx = position.cx,
                    .cy = position.cy,
                    .flags = position.flags,
                };
            }

            if (result.output_size < sizeof(EMU_WINDOWPOS))
            {
                return std::nullopt;
            }

            EMU_WINDOWPOS position{};
            if (!c.win_emu.memory.try_read_memory(result.output, &position, sizeof(position)))
            {
                return std::nullopt;
            }
            return position;
        }

        uint64_t pack_message_lparam(const int low, const int high)
        {
            return static_cast<uint64_t>(static_cast<uint16_t>(low)) | (static_cast<uint64_t>(static_cast<uint16_t>(high)) << 16);
        }

        void apply_window_position_change(const syscall_context& c, window& win, const EMU_WINDOWPOS& position,
                                          emulator_stack_allocation& changed_window_pos_alloc, std::vector<qmsg>& message_queue,
                                          const bool update_changed_window_position = true)
        {
            const auto old_client_x = win.client_x();
            const auto old_client_y = win.client_y();
            const auto old_client_width = win.client_width();
            const auto old_client_height = win.client_height();
            const auto x = (position.flags & SWP_NOMOVE) != 0 ? win.x : position.x;
            const auto y = (position.flags & SWP_NOMOVE) != 0 ? win.y : position.y;
            const auto width = (position.flags & SWP_NOSIZE) != 0 ? win.width : position.cx;
            const auto height = (position.flags & SWP_NOSIZE) != 0 ? win.height : position.cy;

            // TODO: Handle SWP_FRAMECHANGED once runtime style changes and non-client recalculation are implemented.
            update_window_geometry(c, win, x, y, width, height, false);

            EMU_WINDOWPOS changed_position{
                .hwnd = position.hwnd,
                .hwndInsertAfter = position.hwndInsertAfter,
                .x = win.x,
                .y = win.y,
                .cx = win.width,
                .cy = win.height,
                .flags = position.flags & ~(SWP_NOCLIENTMOVE | SWP_NOCLIENTSIZE),
            };
            if (old_client_x == win.client_x() && old_client_y == win.client_y())
            {
                changed_position.flags |= SWP_NOCLIENTMOVE;
            }
            if (old_client_width == win.client_width() && old_client_height == win.client_height())
            {
                changed_position.flags |= SWP_NOCLIENTSIZE;
            }

            if (changed_window_pos_alloc)
            {
                if (!update_changed_window_position)
                {
                    EMU_WINDOWPOS previous_changed_position{};
                    c.emu.read_memory(changed_window_pos_alloc.address(), &previous_changed_position, sizeof(previous_changed_position));
                    changed_position.hwnd = previous_changed_position.hwnd;
                    changed_position.hwndInsertAfter = previous_changed_position.hwndInsertAfter;
                    changed_position.flags = previous_changed_position.flags;
                }
                c.emu.write_memory(changed_window_pos_alloc.address(), &changed_position, sizeof(changed_position));
            }
            else
            {
                changed_window_pos_alloc = c.emu.push_stack(changed_position);
            }

            for (auto& message : message_queue)
            {
                switch (message.message)
                {
                case WM_WINDOWPOSCHANGED:
                    message.lParam = changed_window_pos_alloc.address();
                    break;
                case WM_MOVE:
                    message.lParam = pack_message_lparam(win.client_x(), win.client_y());
                    break;
                case WM_SIZE:
                    message.lParam = pack_message_lparam(win.client_width(), win.client_height());
                    break;
                default:
                    break;
                }
            }
        }

        EMU_WINDOWPOS resolve_window_position_callback(const syscall_context& c, const uint64_t window_pos_address)
        {
            EMU_WINDOWPOS position{};
            c.emu.read_memory(window_pos_address, &position, sizeof(position));
            if (const auto callback_position = read_window_pos_callback_output(c))
            {
                position = *callback_position;
                c.emu.write_memory(window_pos_address, &position, sizeof(position));
            }

            return position;
        }

        void release_window_position_allocations(const syscall_context& c, window_position_state& state)
        {
            if (state.changed_window_pos_alloc)
            {
                c.emu.pop_stack(state.changed_window_pos_alloc);
            }
            if (state.window_pos_alloc)
            {
                c.emu.pop_stack(state.window_pos_alloc);
            }
        }

        void invalidate_window_tree(const syscall_context& c, window& win);

        void apply_set_window_position(const syscall_context& c, window& win, window_position_state& state, const EMU_WINDOWPOS& position)
        {
            const auto old_x = win.x;
            const auto old_y = win.y;
            const auto old_width = win.width;
            const auto old_height = win.height;

            apply_window_position_change(c, win, position, state.changed_window_pos_alloc, state.message_queue);

            const bool showing_hidden_window = (position.flags & SWP_SHOWWINDOW) != 0 && (win.style & WS_VISIBLE) == 0;
            const bool hiding_visible_window = (position.flags & SWP_HIDEWINDOW) != 0 && (win.style & WS_VISIBLE) != 0;

            if ((position.flags & SWP_HIDEWINDOW) != 0)
            {
                win.style &= ~WS_VISIBLE;
                win.guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win.style; });
                if (win.host_surface_window)
                {
                    c.win_emu.ui().set_window_visible(win.handle, false);
                }
            }
            else if (showing_hidden_window)
            {
                win.style |= WS_VISIBLE;
                win.guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = win.style; });
                if (win.host_surface_window)
                {
                    c.win_emu.ui().set_window_visible(win.handle, true);
                }
                invalidate_window_tree(c, win);
            }

            const bool geometry_changed = old_x != win.x || old_y != win.y || old_width != win.width || old_height != win.height;
            if ((position.flags & SWP_NOREDRAW) == 0 && !showing_hidden_window && geometry_changed)
            {
                invalidate_window(c, win, std::nullopt, false);
            }

            // TODO: Apply hwndInsertAfter once guest and host z-order mutation are implemented.

            if (geometry_changed || showing_hidden_window || hiding_visible_window)
            {
                state.message_queue.push_back(
                    {.message = WM_WINDOWPOSCHANGED, .wParam = 0, .lParam = state.changed_window_pos_alloc.address()});
            }
            state.position_applied = true;
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

        // All windows of a top-level window paint into one shared surface, so whenever a window may paint over its
        // children, they have to paint again afterwards. Only WS_CLIPCHILDREN keeps a window's painting off them.
        void invalidate_window(const syscall_context& c, window& win, const std::optional<RECT>& update_rect = std::nullopt,
                               bool erase = false, const child_redraw_scope scope)
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

            if (c.proc.is_window_effectively_visible(win.handle))
            {
                if (auto* thread = c.proc.find_thread_by_id(win.thread_id))
                {
                    thread->mark_queue_status_changed(QS_PAINT);
                }
            }

            if (!reaches_children(win, scope))
            {
                return;
            }

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle != win.handle || (child.style & WS_VISIBLE) == 0)
                {
                    continue;
                }

                if (const auto child_area = area_in_child(child, new_rect))
                {
                    invalidate_window(c, child, child_area, erase, scope);
                }
            }
        }

        void set_internal_paint_pending(const syscall_context& c, window& win, const bool pending)
        {
            win.internal_paint_pending = pending;
            if (pending && c.proc.is_window_effectively_visible(win.handle))
            {
                if (auto* thread = c.proc.find_thread_by_id(win.thread_id))
                {
                    thread->mark_queue_status_changed(QS_PAINT);
                }
            }
        }

        // Invalidate a window together with its visible descendant controls. Used when a window
        // transitions to visible: child controls created while an ancestor was still hidden skip
        // their paint body (user32 gates control painting on the whole parent chain being visible),
        // so they must be repainted once the ancestor becomes visible.
        void invalidate_window_tree(const syscall_context& c, window& win)
        {
            invalidate_window(c, win, std::nullopt, false, child_redraw_scope::all);
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

        uint64_t copy_text_to_buffer(const syscall_context& c, const std::u16string_view text, const uint64_t character_count,
                                     const uint64_t buffer, const bool ansi)
        {
            if (character_count == 0 || buffer == 0)
            {
                return 0;
            }

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

        uint64_t copy_def_window_text(const syscall_context& c, const window& win, const uint64_t character_count, const uint64_t buffer,
                                      const bool ansi)
        {
            return copy_text_to_buffer(c, read_guest_window_text(c, win), character_count, buffer, ansi);
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
            win.erase_pending = false;
            win.update_rect = {};
        }

        // A window's update region is tracked as one bounding rectangle, so a child is only validated when the area covers
        // all of it; validating less would drop the uncovered part of its pending repaint.
        void validate_children(const syscall_context& c, const window& parent, const RECT& area, const child_redraw_scope scope)
        {
            if (!reaches_children(parent, scope))
            {
                return;
            }

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle != parent.handle)
                {
                    continue;
                }

                const auto child_area = area_in_child(child, area);
                if (!child_area)
                {
                    continue;
                }

                const auto& pending = child.update_rect;
                if (child.update_pending && pending.left >= child_area->left && pending.top >= child_area->top &&
                    pending.right <= child_area->right && pending.bottom <= child_area->bottom)
                {
                    validate_window(child);
                }
                validate_children(c, child, *child_area, scope);
            }
        }

        void present_existing_guest_window_surface(const syscall_context& c, const window& painted_window)
        {
            const window* top_level = &painted_window;
            while (top_level && !c.proc.is_surface_root(*top_level))
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

            c.win_emu.ui().present_surface(top_level->handle, make_window_surface_desc(surface, top_level));
        }

        void dispatch_cbt_create_window(const syscall_context& c, window_create_state&& state, const window& win,
                                        const user_thread_hook& hook, const pointer window_name, const pointer class_name)
        {
            fn_hk_in_lp_cbt_create_struct_message args{};
            args.pwnd = win.guest.value();
            args.msg = static_cast<UINT>(k_hcbt_createwnd | (k_wh_cbt << 16));
            args.wParam = win.handle;
            args.hwndInsertAfter = 0;
            args.xpfnProc = hook.proc;
            args.ansi = hook.ansi ? TRUE : FALSE;
            c.emu.read_memory(state.create_struct_alloc.address(), &args.cs, sizeof(args.cs));
            args.cs.lpszName = window_name;
            args.cs.lpszClass = class_name;

            enter_hook(c, hook);
            dispatch_user_callback(c, callback_id::NtUserCreateWindowEx, k_fn_hk_in_lp_cbt_create_struct_callback_id, std::move(state),
                                   args);
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
                        EMU_WINDOWPOS window_pos{};
                        c.emu.read_memory(l_param, &window_pos, sizeof(window_pos));
                        if (c.proc.is_wow64_process)
                        {
                            args.data.window_pos32 = {
                                .hwnd = static_cast<uint32_t>(window_pos.hwnd),
                                .hwndInsertAfter = static_cast<uint32_t>(window_pos.hwndInsertAfter),
                                .x = window_pos.x,
                                .y = window_pos.y,
                                .cx = window_pos.cx,
                                .cy = window_pos.cy,
                                .flags = window_pos.flags,
                            };
                        }
                        else
                        {
                            args.data.window_pos = window_pos;
                        }
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
            if (message_queue.empty())
            {
                throw std::runtime_error("Cannot dispatch the next message because the message queue is empty");
            }

            const auto m = message_queue.back();
            message_queue.pop_back();

            dispatch_window_message(c, id, std::forward<T>(state), win, m.message, m.wParam, m.lParam);
        }

        void complete_window_destroy_step(const syscall_context& c, window_destroy_data& state)
        {
            if (state.frames.empty())
            {
                return;
            }

            auto& frame = state.frames.back();
            if (frame.pending_window_pos_address == 0)
            {
                return;
            }

            if (auto* win = c.proc.windows.get(frame.handle))
            {
                const auto position = resolve_window_position_callback(c, frame.pending_window_pos_address);
                apply_window_position_change(c, *win, position, frame.changed_window_pos_alloc, frame.message_queue, true);
            }
            frame.pending_window_pos_address = 0;
        }

        template <typename State>
        BOOL advance_window_destroy(const syscall_context& c, State& completion_state, window_destroy_data& destruction,
                                    const callback_id completion_callback)
        {
            window_destroy_orchestrator orchestrator{destruction, c};
            const auto step = orchestrator.advance();
            if (!step)
            {
                return TRUE;
            }

            auto* win = c.proc.windows.get(step->handle);
            if (!win)
            {
                return advance_window_destroy(c, completion_state, destruction, completion_callback);
            }

            dispatch_window_message(c, completion_callback, std::move(completion_state), *win, step->message.message, step->message.wParam,
                                    step->message.lParam);
            return {};
        }

        BOOL advance_window_show(const syscall_context& c, window_show_state& state)
        {
            window_show_orchestrator orchestrator{state.data, c};
            const auto step = orchestrator.advance();
            if (!step)
            {
                return state.was_visible ? TRUE : FALSE;
            }

            auto* win = c.proc.windows.get(step->handle);
            if (!win)
            {
                return advance_window_show(c, state);
            }

            dispatch_window_message(c, callback_id::NtUserShowWindow, std::move(state), *win, step->message.message, step->message.wParam,
                                    step->message.lParam);
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

            const auto thread_info = thread->teb64->field<std::uint64_t>(offsetof(TEB64, Win32ThreadInfo)).read();

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

            const auto& teb64 = *thread->teb64;
            teb64.field<std::uint64_t>(offsetof(TEB64, Win32ThreadInfo)).write(thread_info);
            teb64.field<ULONG>(offsetof(TEB64, User32Reserved) + 13 * sizeof(ULONG)).write(low);
            teb64.field<ULONG>(offsetof(TEB64, User32Reserved) + 14 * sizeof(ULONG)).write(high);

            if (c.proc.is_wow64_process && thread->teb32)
            {
                const auto& teb32 = *thread->teb32;
                teb32.field<std::uint32_t>(offsetof(TEB32, Win32ThreadInfo)).write(low);
                teb32.field<ULONG>(offsetof(TEB32, User32Reserved) + 13 * sizeof(ULONG)).write(low);
                teb32.field<ULONG>(offsetof(TEB32, User32Reserved) + 14 * sizeof(ULONG)).write(high);
            }
        }

        NTSTATUS ensure_user_shared_info_ptr(const syscall_context& c, uint64_t& user_shared_info_ptr)
        {
            user_shared_info_ptr = 0;

            if (!c.proc.peb32)
            {
                return STATUS_SUCCESS;
            }

            user_shared_info_ptr = c.proc.peb32->field<std::uint32_t>(offsetof(PEB32, UserSharedInfoPtr)).read();

            if (user_shared_info_ptr != 0)
            {
                return STATUS_SUCCESS;
            }

            user_shared_info_ptr = c.proc.base_allocator.reserve(sizeof(USER_SHAREDINFO), alignof(USER_SHAREDINFO));
            std::array<std::byte, sizeof(USER_SHAREDINFO)> zeros{};
            c.emu.write_memory(user_shared_info_ptr, zeros.data(), zeros.size());

            uint32_t user_shared_info_ptr32{};
            const auto narrow_status = win32k_userconnect::narrow_wow64_address(user_shared_info_ptr, user_shared_info_ptr32);
            if (narrow_status != STATUS_SUCCESS)
            {
                return narrow_status;
            }

            c.proc.peb32->field<std::uint32_t>(offsetof(PEB32, UserSharedInfoPtr)).write(user_shared_info_ptr32);
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

        struct class_argument
        {
            std::u16string name{};
            std::optional<uint16_t> atom{};
        };

        class_argument read_class_argument(const syscall_context& c,
                                           const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name)
        {
            if (!class_name)
            {
                return {};
            }

            if (class_name.value() <= std::numeric_limits<uint16_t>::max())
            {
                return {.name = {}, .atom = static_cast<uint16_t>(class_name.value())};
            }

            if (const auto string = class_name.read();
                string.Length == 0 && string.Buffer != 0 && string.Buffer <= std::numeric_limits<uint16_t>::max())
            {
                return {.name = {}, .atom = static_cast<uint16_t>(string.Buffer)};
            }

            return {.name = read_unicode_string(c.emu, class_name), .atom = std::nullopt};
        }

        process_context::class_entry* find_class_by_atom(const syscall_context& c, const uint16_t atom)
        {
            for (auto& entry : c.proc.classes | std::views::values)
            {
                if (entry.atom == atom)
                {
                    return &entry;
                }
            }

            return nullptr;
        }

        process_context::class_entry* find_registered_class(const syscall_context& c, const class_argument& cls)
        {
            if (cls.atom)
            {
                return find_class_by_atom(c, *cls.atom);
            }

            const auto it = c.proc.classes.find(cls.name);
            return it != c.proc.classes.end() ? &it->second : nullptr;
        }

        process_context::class_entry* find_class(const syscall_context& c, const class_argument& cls)
        {
            if (!cls.atom)
            {
                return ensure_builtin_window_class(c, cls.name);
            }

            if (*cls.atom == builtin_dialog_class_atom)
            {
                return ensure_builtin_window_class(c, builtin_dialog_class_name);
            }

            return find_class_by_atom(c, *cls.atom);
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

        std::optional<uint64_t> convert_create_struct_text(const syscall_context& c, const emulator_object<LARGE_STRING> text,
                                                           const bool ansi)
        {
            if (!text || text.value() <= std::numeric_limits<uint16_t>::max())
            {
                return std::nullopt;
            }

            const auto str = text.read();
            if (str.Buffer == 0 || (str.bAnsi != 0) == ansi)
            {
                return std::nullopt;
            }

            const auto decoded = read_large_string(text);
            const auto write = [&](const void* data, const size_t size) {
                const auto address =
                    c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(size)), memory_permission::read_write);
                c.win_emu.memory.write_memory(address, data, size);
                return address;
            };

            if (ansi)
            {
                const auto narrow = u16_to_cp1252(decoded);
                return write(narrow.c_str(), narrow.size() + 1);
            }

            return write(decoded.c_str(), (decoded.size() + 1) * sizeof(char16_t));
        }

        void release_converted_create_struct_text(const syscall_context& c, window_create_state& state)
        {
            for (const auto address : state.converted_create_struct_text)
            {
                c.win_emu.memory.release_memory(address, 0);
            }

            state.converted_create_struct_text.clear();
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
            return handle.bits;
        }

        bool destroy_menu_tree(const syscall_context& c, const hmenu handle, std::unordered_set<hmenu>& visited)
        {
            if (!visited.insert(handle).second)
            {
                return false;
            }

            auto* menu_obj = c.proc.menus.get(handle);
            if (!menu_obj)
            {
                return false;
            }

            for (const auto& item : menu_obj->items)
            {
                if (item.submenu != 0)
                {
                    destroy_menu_tree(c, item.submenu, visited);
                }
            }

            menu_obj->release_guest_backing(c.win_emu.memory);
            return c.proc.menus.erase(handle);
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
        uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                          emulator_pointer bits);
        hdc create_gdi_window_dc(const syscall_context& c, hwnd window);
        bool is_gdi_brush(const syscall_context& c, uint64_t handle);
        uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
        uint32_t count_process_gdi_objects(const syscall_context& c);
        bool set_gdi_region_rect(const syscall_context& c, handle region, const RECT& rect);
        BOOL handle_NtGdiFlush(const syscall_context& c);
        BOOL handle_NtGdiPatBlt(const syscall_context& c, hdc dc, LONG x, LONG y, LONG width, LONG height, DWORD rop);
        uint64_t handle_NtGdiSelectBrushLocal(const syscall_context& c, hdc dc, uint32_t brush, emulator_pointer old_brush_ptr);
        gdi_bitmap_surface* get_dc_present_surface(const syscall_context& c, hdc dc, uint32_t& present_handle);
        void draw_system_button_glyph(const syscall_context& c, hdc dc, int x, int y, uint32_t index);
        BOOL handle_NtUserRemoveMenu(const syscall_context& c, hmenu menu, UINT position, UINT flags);
        BOOL handle_NtUserSetDialogPointer(const syscall_context& c, hwnd hwnd, emulator_pointer ptr);
        BOOL handle_NtUserSetDialogSystemMenu(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserSetMsgBox(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserUpdateWindow(const syscall_context& c, hwnd hwnd);
        BOOL handle_NtUserPostQuitMessage(const syscall_context& c, int exit_code);

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

            constexpr size_t max_name_length = 255;

            const auto raw = message_name.try_read();
            if (!raw || raw->Buffer == 0 || raw->Length > raw->MaximumLength || (raw->Length & 1) != 0)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            if (raw->Length == 0)
            {
                return 0;
            }

            if (raw->Length > max_name_length * sizeof(char16_t))
            {
                set_guest_last_error(c, 234); // ERROR_MORE_DATA
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
            if (routine == k_thread_state_send_message)
            {
                const auto* thread = c.vcpu.active_thread;
                if (!thread || thread->active_sent_messages.empty())
                {
                    return 0;
                }

                return k_ismex_send | (thread->active_sent_messages.back().replied ? k_ismex_replied : 0);
            }

            if (routine == k_thread_state_message_time)
            {
                return c.vcpu.active_thread ? c.vcpu.active_thread->current_message_time : 0;
            }

            if (routine == k_thread_state_dialog_state)
            {
                return c.vcpu.active_thread ? c.vcpu.active_thread->win32k_thread_state : 0;
            }

            if (routine == k_thread_state_capture_window)
            {
                const auto* captured = c.proc.windows.get(c.proc.mouse_capture_window);
                return captured && c.vcpu.active_thread && captured->thread_id == c.vcpu.active_thread->id ? c.proc.mouse_capture_window
                                                                                                           : 0;
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

            if (!win32k_userconnect::try_write_user_shared_info(c.emu, connect_destination, c.proc))
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
                if (!win32k_userconnect::try_write_user_shared_info(c.emu, user_shared_info_ptr, c.proc))
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

        // The station's object identity for scoping desktop names. The interactive station is not in the store, so it gets a
        // stable pseudo reference; a stored station is keyed by its index, not by whichever handle value names it.
        object_reference window_station_reference(const syscall_context& c, const uint64_t station)
        {
            if (station == interactive_window_station)
            {
                return object_reference{.type = handle_types::window_station,
                                        .pseudo = make_pseudo_handle(1, handle_types::window_station)};
            }

            return c.proc.window_stations.reference_of(make_handle(station)).value_or(object_reference{});
        }

        handle default_desktop(const syscall_context& c)
        {
            if (!c.proc.default_desktop)
            {
                desktop desk{};
                desk.name = u"Default";
                desk.window_station = window_station_reference(c, interactive_window_station);
                auto [handle, _] = c.proc.desktops.store_and_get(std::move(desk));
                c.proc.default_desktop = c.proc.desktops.retain(handle).value_or(object_reference{});
                return handle;
            }

            return c.proc.desktops.make_handle(c.proc.default_desktop.index);
        }

        object_reference resolve_desktop(const syscall_context& c, const handle desktop_handle)
        {
            const auto reference = c.proc.reference_of(desktop_handle);
            const auto* desk = c.proc.get_object_as<desktop>(reference);
            return desk && desk->opened_desktop ? desk->opened_desktop : reference;
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
                target->win32k_desktop = default_desktop(c);
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

            auto window = it->second.target_window;
            if (window == 0 && it->second.is_window_dc)
            {
                window = c.proc.default_desktop_window_handle.bits;
            }
            if (window == 0 || !c.proc.windows.get(window))
            {
                return 0;
            }

            return window;
        }

        uint64_t system_control_brush(const syscall_context& c, const uint32_t control_type)
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

        // Controls ask for their brush with a WM_CTLCOLOR* message, which is WM_CTLCOLORMSGBOX plus the CTLCOLOR_* type.
        constexpr uint32_t k_wm_ctlcolormsgbox = 0x132;
        constexpr uint32_t k_wm_ctlcolorstatic = 0x138;

        bool is_control_color_message(const uint32_t message)
        {
            return message >= k_wm_ctlcolormsgbox && message <= k_wm_ctlcolorstatic;
        }

        uint64_t control_message_brush(const syscall_context& c, const uint32_t message)
        {
            return is_control_color_message(message) ? system_control_brush(c, message - k_wm_ctlcolormsgbox) : 0;
        }

        // The parent picks a control's brush by answering the WM_CTLCOLOR* message, with the DC in wParam and the control in
        // lParam; only a parent that returns no brush leaves the control with the system one.
        bool ask_parent_for_control_brush(const syscall_context& c, const callback_id id, const hwnd parent, const hwnd window,
                                          const hdc dc, const uint32_t message)
        {
            const auto* parent_window = c.proc.windows.get(parent);
            if (!is_control_color_message(message) || !parent_window || parent_window->wnd_proc == 0 ||
                parent_window->thread_id != c.thread().id)
            {
                return false;
            }

            dispatch_window_message(c, id, nullptr, *parent_window, message, dc, window);
            return true;
        }

        uint64_t answered_control_brush(const syscall_context& c, const uint32_t message)
        {
            const auto brush = c.get_callback_result<uint64_t>();
            return is_gdi_brush(c, brush) ? brush : control_message_brush(c, message);
        }

        // A child control asks its parent; a top-level window answers for itself.
        hwnd control_brush_parent(const syscall_context& c, const hwnd window)
        {
            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return 0;
            }
            return (win->style & WS_CHILD) != 0 && win->parent_handle != 0 ? win->parent_handle : window;
        }

        uint64_t handle_NtUserGetControlBrush(const syscall_context& c, const hwnd window, const hdc dc, const uint32_t message)
        {
            if (ask_parent_for_control_brush(c, callback_id::NtUserGetControlBrush, control_brush_parent(c, window), window, dc, message))
            {
                return {};
            }
            return control_message_brush(c, message);
        }

        uint64_t completion_NtUserGetControlBrush(const syscall_context& c, const hwnd /*window*/, const hdc /*dc*/, const uint32_t message)
        {
            return answered_control_brush(c, message);
        }

        uint64_t handle_NtUserGetControlColor(const syscall_context& c, const hwnd parent, const hwnd window, const hdc dc,
                                              const uint32_t message)
        {
            if (ask_parent_for_control_brush(c, callback_id::NtUserGetControlColor, parent, window, dc, message))
            {
                return {};
            }
            return control_message_brush(c, message);
        }

        uint64_t completion_NtUserGetControlColor(const syscall_context& c, const hwnd /*parent*/, const hwnd /*window*/, const hdc /*dc*/,
                                                  const uint32_t message)
        {
            return answered_control_brush(c, message);
        }

        constexpr hbrush k_ctlcolor_max = 7;

        BOOL fill_window_client(const syscall_context& c, const hwnd window, const hdc dc, const uint64_t control_brush)
        {
            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            constexpr DWORD patcopy = 0x00F00021;
            const auto previous_brush = handle_NtGdiSelectBrushLocal(c, dc, static_cast<uint32_t>(control_brush), 0);
            const auto client_rect = get_client_rect(*win);
            const auto result = handle_NtGdiPatBlt(c, dc, client_rect.left, client_rect.top, client_rect.right - client_rect.left,
                                                   client_rect.bottom - client_rect.top, patcopy);
            handle_NtGdiSelectBrushLocal(c, dc, static_cast<uint32_t>(previous_brush), 0);
            return result;
        }

        // A CTLCOLOR_* index instead of a brush fills with the brush the parent picks for that kind of control.
        BOOL handle_NtUserFillWindow(const syscall_context& c, const hwnd parent_window, const hwnd window, const hdc dc,
                                     const hbrush brush)
        {
            if (dc == 0 || !c.proc.windows.get(window) || (parent_window != 0 && !c.proc.windows.get(parent_window)))
            {
                return FALSE;
            }

            if (brush >= k_ctlcolor_max)
            {
                return fill_window_client(c, window, dc, brush);
            }

            const auto message = k_wm_ctlcolormsgbox + static_cast<uint32_t>(brush);
            if (ask_parent_for_control_brush(c, callback_id::NtUserFillWindow, parent_window, window, dc, message))
            {
                return {};
            }
            return fill_window_client(c, window, dc, control_message_brush(c, message));
        }

        BOOL completion_NtUserFillWindow(const syscall_context& c, const hwnd /*parent_window*/, const hwnd window, const hdc dc,
                                         const hbrush brush)
        {
            return fill_window_client(c, window, dc, answered_control_brush(c, k_wm_ctlcolormsgbox + static_cast<uint32_t>(brush)));
        }

        BOOL handle_NtUserReleaseDC(const syscall_context& c, const hdc dc)
        {
            // TODO: Native GDI doesn't require ReleaseDC for window-DC writes to become visible; presenting here is only
            //       a Sogen flush boundary. Move presentation into the relevant window-DC drawing paths once unified.
            uint32_t present_handle = 0;
            if (auto* surface = get_dc_present_surface(c, dc, present_handle);
                surface && present_handle != 0 && surface->width > 0 && surface->height > 0 && !surface->pixels.empty())
            {
                c.win_emu.ui().present_surface(present_handle, make_window_surface_desc(*surface, c.proc.windows.get(present_handle)));
            }

            (void)handle_NtGdiDeleteObjectApp(c, static_cast<uint32_t>(dc));
            return TRUE;
        }

        hwnd handle_NtUserSetCapture(const syscall_context& c, const hwnd window)
        {
            const auto previous = c.proc.mouse_capture_window;
            if (window == 0 || c.proc.windows.get(window))
            {
                c.proc.set_mouse_capture(window);
            }
            return previous;
        }

        BOOL handle_NtUserReleaseCapture(const syscall_context& c)
        {
            c.proc.set_mouse_capture(0);
            return TRUE;
        }

        // A registration replaces an earlier one of the same usage page and usage, and RIDEV_REMOVE drops it. They are kept in
        // the order NtUserGetRegisteredRawInputDevices reports them in.
        void register_raw_input_device(std::vector<process_context::raw_input_registration>& registrations, const RAWINPUTDEVICE32& device)
        {
            const auto position = std::ranges::lower_bound(registrations, std::pair{device.usUsagePage, device.usUsage}, {},
                                                           [](const process_context::raw_input_registration& registration) {
                                                               return std::pair{registration.usage_page, registration.usage};
                                                           });
            const bool exists =
                position != registrations.end() && position->usage_page == device.usUsagePage && position->usage == device.usUsage;

            if ((device.dwFlags & RIDEV_REMOVE) != 0)
            {
                if (exists)
                {
                    registrations.erase(position);
                }
                return;
            }

            const process_context::raw_input_registration registration{
                .usage_page = device.usUsagePage, .usage = device.usUsage, .flags = device.dwFlags, .target = device.hwndTarget};
            if (exists)
            {
                *position = registration;
            }
            else
            {
                registrations.insert(position, registration);
            }
        }

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

                register_raw_input_device(c.proc.raw_input_registrations, device);
            }

            c.win_emu.apply_cursor_capture();
            return TRUE;
        }

        bool write_registered_device_count(const syscall_context& c, const emulator_pointer device_count, const uint32_t count)
        {
            return is_writable_range(c.win_emu, device_count, sizeof(count)) &&
                   c.win_emu.memory.try_write_memory(device_count, &count, sizeof(count));
        }

        // On success the registrations are written in the order of usage page and usage, and the count stays as given, so only
        // the failing paths need it writable. The whole buffer the count names must be writable, even when it is too small.
        uint32_t handle_NtUserGetRegisteredRawInputDevices(const syscall_context& c, const emulator_pointer devices,
                                                           const emulator_pointer device_count, const uint32_t size)
        {
            constexpr auto failed = static_cast<uint32_t>(-1);
            constexpr DWORD error_invalid_parameter = 87;
            constexpr DWORD error_insufficient_buffer = 122;
            constexpr DWORD error_noaccess = 998;

            if (size != sizeof(RAWINPUTDEVICE64))
            {
                set_guest_last_error(c, error_invalid_parameter);
                return failed;
            }

            uint32_t capacity{};
            if (!c.win_emu.memory.try_read_memory(device_count, &capacity, sizeof(capacity)))
            {
                set_guest_last_error(c, error_noaccess);
                return failed;
            }

            const auto& registrations = c.proc.raw_input_registrations;
            const auto needed = static_cast<uint32_t>(registrations.size());
            if (devices == 0)
            {
                if (!write_registered_device_count(c, device_count, needed))
                {
                    set_guest_last_error(c, error_noaccess);
                    return failed;
                }
                return 0;
            }

            if (capacity == 0)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return failed;
            }

            if (!is_writable_range(c.win_emu, devices, static_cast<uint64_t>(capacity) * sizeof(RAWINPUTDEVICE64)))
            {
                set_guest_last_error(c, error_noaccess);
                return failed;
            }

            if (capacity < needed)
            {
                const bool reported = write_registered_device_count(c, device_count, needed);
                set_guest_last_error(c, reported ? error_insufficient_buffer : error_noaccess);
                return failed;
            }

            for (uint32_t i = 0; i < needed; ++i)
            {
                const auto& registration = registrations[i];
                const RAWINPUTDEVICE64 device{.usUsagePage = registration.usage_page,
                                              .usUsage = registration.usage,
                                              .dwFlags = registration.flags,
                                              .hwndTarget = registration.target};
                c.win_emu.memory.write_memory(devices + (static_cast<uint64_t>(i) * sizeof(device)), &device, sizeof(device));
            }

            return needed;
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

        // GetRawInputData fetches the payload referenced by WM_INPUT's lParam. RID_HEADER reads only the header,
        // RID_INPUT reads the header and payload, and a null data pointer queries the required size. Reads leave
        // the token valid so the window procedure can read the payload again.
        //
        // The mouse or keyboard body follows the header with no extra padding. Native 64-bit callers use the
        // 24-byte header, while WoW64 callers can use either the 16-byte or 24-byte layout.
        uint32_t handle_NtUserGetRawInputData(const syscall_context& c, const emulator_pointer raw_input, const uint32_t command,
                                              const emulator_pointer data, const emulator_object<uint32_t> size_ptr,
                                              const uint32_t header_size)
        {
            constexpr auto failure = static_cast<uint32_t>(-1);
            const auto valid_header =
                header_size == sizeof(RAWINPUTHEADER64) || (header_size == sizeof(RAWINPUTHEADER32) && c.proc.is_wow64_process);
            if (!size_ptr || !valid_header)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return failure;
            }

            const auto* payload = c.thread().find_raw_input(raw_input);
            if (!payload)
            {
                set_guest_last_error(c, 6); // ERROR_INVALID_HANDLE
                return failure;
            }

            if (command != RID_HEADER && command != RID_INPUT)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return failure;
            }

            // The payload is a RAWKEYBOARD or RAWMOUSE depending on what the token carried.
            const uint32_t body_size = payload->keyboard ? sizeof(RAWKEYBOARD32) : sizeof(RAWMOUSE32);
            const uint32_t dw_type = payload->keyboard ? RIM_TYPEKEYBOARD : RIM_TYPEMOUSE;

            const uint32_t total = header_size + body_size;
            const uint32_t required = (command == RID_HEADER) ? header_size : total;

            if (data == 0)
            {
                size_ptr.write(required);
                return 0;
            }

            if (size_ptr.read() < required)
            {
                size_ptr.write(required);
                set_guest_last_error(c, 122); // ERROR_INSUFFICIENT_BUFFER
                return failure;
            }

            // dwType @0 and dwSize @4 are identical across header layouts; hDevice/wParam stay zeroed, so only
            // the overall header size (which shifts the body) matters.
            std::array<uint8_t, sizeof(RAWINPUTHEADER64) + sizeof(RAWMOUSE32)> buffer{};
            const uint32_t dw_size = total;
            std::memcpy(buffer.data() + 0, &dw_type, sizeof(dw_type));
            std::memcpy(buffer.data() + 4, &dw_size, sizeof(dw_size));
            if (command == RID_INPUT)
            {
                if (payload->keyboard)
                {
                    RAWKEYBOARD32 keyboard{};
                    keyboard.MakeCode = payload->scan_code;
                    const bool key_release = payload->key_message == WM_KEYUP || payload->key_message == WM_SYSKEYUP;
                    keyboard.Flags =
                        static_cast<uint16_t>((key_release ? RI_KEY_BREAK : RI_KEY_MAKE) | (payload->key_extended ? RI_KEY_E0 : 0));
                    keyboard.VKey = payload->vkey;
                    keyboard.Message = payload->key_message;
                    std::memcpy(buffer.data() + header_size, &keyboard, sizeof(keyboard));
                }
                else
                {
                    RAWMOUSE32 mouse{};
                    mouse.usFlags = MOUSE_MOVE_RELATIVE;
                    // RAWMOUSE has a usButtonFlags/usButtonData union packed into ulButtons. The high
                    // word carries wheel delta for RI_MOUSE_WHEEL/RI_MOUSE_HWHEEL.
                    mouse.ulButtons =
                        static_cast<uint32_t>(payload->mouse_buttons) | (static_cast<uint32_t>(payload->mouse_button_data) << 16);
                    mouse.lLastX = payload->dx;
                    mouse.lLastY = payload->dy;
                    std::memcpy(buffer.data() + header_size, &mouse, sizeof(mouse));
                }
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
            auto* win = c.proc.windows.get(window);
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
                ps.rcPaint = win->update_pending ? win->update_rect : RECT{};
                ps.fRestore = FALSE;
                ps.fIncUpdate = FALSE;
                paint_struct.write(ps);
            }

            validate_window(*win);
            win->internal_paint_pending = false;
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
                    c.win_emu.ui().present_surface(present_handle, make_window_surface_desc(*surface, c.proc.windows.get(present_handle)));
                }

                // BeginPaint allocated a fresh GDI DC (handle table entry + DC_ATTR block) via
                // GetDCEx; fully delete it here so repeated repaints don't leak GDI handles.
                (void)handle_NtGdiDeleteObjectApp(c, static_cast<uint32_t>(ps.paint_hdc));
            }

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

            RECT rect{
                .left = 0, .top = 0, .right = static_cast<LONG>(c.proc.display_width), .bottom = static_cast<LONG>(c.proc.display_height)};
            if (c.proc.cursor_clip)
            {
                rect = *c.proc.cursor_clip;
            }
            else
            {
                const auto display_info = c.proc.user_handles.get_display_info().read();
                if (const emulator_object<USER_MONITOR> monitor_obj(c.emu, display_info.pPrimaryMonitor); monitor_obj)
                {
                    rect = monitor_obj.read().rcMonitor;
                }
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
        BOOL handle_NtUserSetCursorPos(const syscall_context& c, int32_t x, int32_t y)
        {
            if (const auto& clip = c.proc.cursor_clip)
            {
                x = std::clamp(x, static_cast<int32_t>(clip->left),
                               std::max(static_cast<int32_t>(clip->left), static_cast<int32_t>(clip->right) - 1));
                y = std::clamp(y, static_cast<int32_t>(clip->top),
                               std::max(static_cast<int32_t>(clip->top), static_cast<int32_t>(clip->bottom) - 1));
            }

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

        BOOL handle_NtUserClipCursor(const syscall_context& c, const emulator_pointer rect_ptr)
        {
            if (rect_ptr == 0)
            {
                c.proc.cursor_clip.reset();
            }
            else
            {
                auto rect = c.emu.read_memory<RECT>(rect_ptr);
                if (rect.left > rect.right)
                {
                    std::swap(rect.left, rect.right);
                }
                if (rect.top > rect.bottom)
                {
                    std::swap(rect.top, rect.bottom);
                }
                c.proc.cursor_clip = rect;
            }

            c.win_emu.apply_cursor_capture();
            return TRUE;
        }

        BOOL handle_NtUserGetWindowCompositionAttribute(const syscall_context& c, const hwnd window,
                                                        const emulator_object<USER_WINDOWCOMPOSITIONATTRIBDATA> attribute_data)
        {
            if (!attribute_data)
            {
                return FALSE;
            }

            const auto data = attribute_data.read();
            uint32_t attrib = data.Attrib;
            uint64_t pv_data = data.pvData;
            uint64_t cb_data = data.cbData;

            if (pv_data == 0 || cb_data == 0)
            {
                return FALSE;
            }

            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            switch (attrib)
            {
            case WCA_NCRENDERING_ENABLED: {
                if (cb_data < sizeof(uint32_t))
                {
                    return FALSE;
                }

                constexpr BOOL value = TRUE;
                c.emu.write_memory(pv_data, &value, sizeof(value));
                return TRUE;
            }
            case WCA_CLOAKED: {
                if (cb_data < sizeof(uint32_t))
                {
                    return FALSE;
                }

                constexpr uint32_t value = 0;
                c.emu.write_memory(pv_data, &value, sizeof(value));
                return TRUE;
            }

            case WCA_EXTENDED_FRAME_BOUNDS: {
                if (cb_data < sizeof(RECT))
                {
                    return FALSE;
                }

                const auto rect = get_screen_window_rect(c, *win);
                c.emu.write_memory(pv_data, &rect, sizeof(rect));
                return TRUE;
            }

            default:
                c.win_emu.log.warn("Unsupported window composition attribute: %u\n", attrib);
                return FALSE;
            }
        }

        BOOL handle_NtUserSetWindowCompositionAttribute(const syscall_context& c, const hwnd window,
                                                        const emulator_object<USER_WINDOWCOMPOSITIONATTRIBDATA> attribute_data)
        {
            if (!attribute_data)
            {
                return FALSE;
            }

            const auto data = attribute_data.read();
            uint32_t attrib = data.Attrib;
            uint64_t pv_data = data.pvData;
            uint64_t cb_data = data.cbData;

            if (pv_data == 0 || cb_data == 0)
            {
                return FALSE;
            }

            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            if (attrib == WCA_NCRENDERING_ENABLED || attrib == WCA_NCRENDERING_POLICY)
            {
                // We don't track DWM state right now, so let's at least report the current hard-coded value.
                sogen::msg queued_message{};
                queued_message.window = window;
                queued_message.message = WM_DWMNCRENDERINGCHANGED;
                queued_message.wParam = 1;
                queued_message.lParam = 0;
                c.vcpu.active_thread->post_message(c.win_emu, queued_message);
            }

            return TRUE;
        }

        // GetKeyState / GetAsyncKeyState report whether a virtual key (or mouse button) is currently down.
        // Games poll these for in-game input instead of consuming WM_KEYDOWN messages. GetKeyState reports the
        // high down bit and the low toggle bit; GetAsyncKeyState instead returns the low pressed-since-last-query bit.
        //
        // Like win32k, every NtUserGetKeyState also refreshes the caller's CLIENTINFO key cache (dwKeyCache at
        // +0x70, then two bits a key, down and toggled, for the keys below 0x20), from which user32!GetKeyState
        // answers those keys without a syscall until the next key change (process_context::key_state_changed).
        uint32_t handle_NtUserGetKeyState(const syscall_context& c, const int32_t virtual_key)
        {
            if (static_cast<uint32_t>(virtual_key) >= 0x100)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            constexpr uint64_t client_info_key_cache = 0x70;

            struct
            {
                uint32_t generation;
                std::array<uint8_t, 8> states;
            } cache{};

            cache.generation = c.win_emu.memory.read_memory<uint32_t>(c.proc.user_handles.get_server_info().value() +
                                                                      offsetof(USER_SERVERINFO, keyStateGeneration));
            for (uint32_t key = 0; key < 0x20; ++key)
            {
                const auto bits = ((c.proc.key_state[key] & 0x80) ? 1u : 0u) | ((c.proc.key_state[key] & 0x01) ? 2u : 0u);
                cache.states[key >> 2] |= static_cast<uint8_t>(bits << (2 * (key & 3)));
            }
            c.win_emu.memory.write_memory(c.vcpu.active_thread->teb64->value() + offsetof(TEB64, Win32ClientInfo) + client_info_key_cache,
                                          &cache, sizeof(cache));

            const auto state = c.proc.key_state[static_cast<uint32_t>(virtual_key)];
            return ((state & 0x80) ? 0xFF80u : 0u) | (state & 0x01);
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
            c.win_emu.apply_cursor_capture();
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

        NTSTATUS handle_NtUserHideCursorNoCapture()
        {
            // Mouse Vanish defaults to off and is not configurable in the emulated system.
            return STATUS_SUCCESS;
        }

        hcursor handle_NtUserGetCursor(const syscall_context& c)
        {
            return c.proc.current_cursor;
        }

        namespace
        {
            constexpr uint16_t k_cursor_type_cursor = 1;
            constexpr uint16_t k_cursor_type_icon = 3;
            constexpr uint16_t k_oic_sample = 32512;
            constexpr uint16_t k_oic_shield = 32518;
            constexpr uint16_t k_ocr_normal = 32512;
            constexpr uint16_t k_ocr_up = 32516;
            constexpr uint16_t k_idc_sizenwse = 32642;
            constexpr uint16_t k_idc_sizeall = 32646;
            constexpr uint16_t k_idc_no = 32648;
            constexpr uint16_t k_idc_last_system = 32652;
            constexpr uint16_t k_idc_pin = 32671;
            constexpr uint16_t k_idc_person = 32672;
            constexpr uint32_t k_cursorf_lrshared = 0x4;
            constexpr DWORD k_error_invalid_cursor_handle = 1402;
            constexpr uint32_t k_stock_cursor_size = 32;
            constexpr std::u16string_view k_stock_cursor_module = u"USER32";

            struct cursor_resource
            {
                std::u16string name{};
                uint16_t id{};
            };

            std::u16string read_cursor_string(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> string)
            {
                if (!string || string.read().Length == 0)
                {
                    return {};
                }

                return read_unicode_string(c.emu, string);
            }

            cursor_resource read_cursor_resource(const syscall_context& c,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> resource)
            {
                if (resource)
                {
                    const auto string = resource.read();
                    if (string.Length == 0 && string.Buffer <= std::numeric_limits<uint16_t>::max())
                    {
                        return {.name = {}, .id = static_cast<uint16_t>(string.Buffer)};
                    }
                }

                return {.name = read_cursor_string(c, resource), .id = 0};
            }

            uint64_t copy_cursor_bitmap(const syscall_context& c, const hbitmap source, const uint32_t max_rows)
            {
                const auto source_it = c.proc.gdi_bitmap_surfaces.find(static_cast<uint32_t>(source));
                if (source_it == c.proc.gdi_bitmap_surfaces.end())
                {
                    return 0;
                }

                const auto width = source_it->second.width;
                const auto height = std::min(source_it->second.height, max_rows);
                const auto bits_pixel = source_it->second.guest_bpp;

                const auto copy = handle_NtGdiCreateBitmap(c, width, height, 1, bits_pixel, 0);
                const auto copy_it = c.proc.gdi_bitmap_surfaces.find(static_cast<uint32_t>(copy));
                if (copy_it == c.proc.gdi_bitmap_surfaces.end())
                {
                    return 0;
                }

                const auto& pixels = source_it->second.pixels;
                const auto count = std::min(pixels.size(), static_cast<size_t>(width) * height);
                copy_it->second.pixels.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(count));
                return copy;
            }

            uint16_t cursor_name_length(const UNICODE_STRING<EmulatorTraits<Emu64>>& string, const std::u16string& name)
            {
                return static_cast<uint16_t>(std::min<size_t>(name.size(), string.MaximumLength > 0 ? string.MaximumLength - 1 : 0));
            }

            bool is_cursor_name_writable(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> target,
                                         const std::u16string& name)
            {
                if (!target)
                {
                    return true;
                }

                const auto string = target.read();
                if (string.MaximumLength == 0)
                {
                    return true;
                }

                return is_writable_range(c.win_emu, string.Buffer, (cursor_name_length(string, name) + 1) * sizeof(char16_t));
            }

            // Length and MaximumLength count characters here: user32 sizes its WCHAR buffers with them.
            void write_cursor_name(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> target,
                                   const std::u16string& name)
            {
                if (!target)
                {
                    return;
                }

                auto string = target.read();
                const auto length = cursor_name_length(string, name);
                if (string.MaximumLength > 0)
                {
                    c.emu.write_memory(string.Buffer, name.data(), length * sizeof(char16_t));
                    c.emu.write_memory(string.Buffer + length * sizeof(char16_t), u"\0", sizeof(char16_t));
                }

                string.Length = length;
                target.write(string);
            }

            bool is_system_resource_id(const uint16_t type, const uint16_t id)
            {
                if (type == k_cursor_type_icon)
                {
                    return id >= k_oic_sample && id <= k_oic_shield;
                }

                return type == k_cursor_type_cursor &&
                       ((id >= k_ocr_normal && id <= k_ocr_up) || (id >= k_idc_sizenwse && id <= k_idc_sizeall) ||
                        (id >= k_idc_no && id <= k_idc_last_system) || id == k_idc_pin || id == k_idc_person);
            }

            hicon create_stock_cursor(const syscall_context& c, const uint16_t type, cursor_resource resource)
            {
                auto [handle, cursor] = c.proc.cursor_icons.create(c.win_emu.memory);
                cursor.module_name = k_stock_cursor_module;
                cursor.resource_name = std::move(resource.name);
                cursor.resource_id = resource.id;
                cursor.type = type;
                cursor.flags = k_cursorf_lrshared;
                cursor.bpp = 32;
                cursor.width = k_stock_cursor_size;
                cursor.height = 2 * k_stock_cursor_size;
                cursor.mask = handle_NtGdiCreateBitmap(c, cursor.width, cursor.height, 1, 1, 0);
                cursor.color = handle_NtGdiCreateBitmap(c, cursor.width, k_stock_cursor_size, 1, 32, 0);
                if (cursor.type == k_cursor_type_icon)
                {
                    cursor.hotspot_x = k_stock_cursor_size / 2;
                    cursor.hotspot_y = k_stock_cursor_size / 2;
                }

                return handle.bits;
            }
        }

        hcursor handle_NtUserSetCursor(const syscall_context& c, const hcursor cursor)
        {
            const auto previous = std::exchange(c.proc.current_cursor, cursor);
            c.win_emu.apply_cursor_shape();
            // SetCursor(NULL) removes the cursor shape (hides it); a non-null handle restores it.
            c.proc.cursor_shape_visible = cursor != 0;
            apply_cursor_visibility(c);
            return previous;
        }

        hicon handle_NtUserCreateEmptyCursorObject(const syscall_context& c)
        {
            return c.proc.cursor_icons.create(c.win_emu.memory).first.bits;
        }

        BOOL handle_NtUserSetCursorIconData()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetCursorIconDataEx(const syscall_context& c, const hicon icon,
                                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> module_name,
                                              const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> resource,
                                              const emulator_object<EMU_CURSORDATA> data, const int /*thread_cursor_size*/)
        {
            auto* cursor = c.proc.cursor_icons.get(icon);
            if (!cursor || !data)
            {
                return FALSE;
            }

            const auto cursor_data = data.read();
            auto resource_key = read_cursor_resource(c, resource);

            cursor->module_name = read_cursor_string(c, module_name);
            cursor->resource_name = std::move(resource_key.name);
            cursor->resource_id = resource_key.id;
            cursor->type = cursor_data.rt;
            cursor->flags = cursor_data.flags;
            cursor->hotspot_x = cursor_data.xHotspot;
            cursor->hotspot_y = cursor_data.yHotspot;
            cursor->mask = cursor_data.hbmMask;
            cursor->color = cursor_data.hbmColor;
            cursor->bpp = cursor_data.bpp;
            cursor->width = cursor_data.cx;
            cursor->height = cursor_data.cy;
            return TRUE;
        }

        BOOL handle_NtUserGetRequiredCursorSizes()
        {
            return TRUE;
        }

        hicon handle_NtUserFindExistingCursorIcon(const syscall_context& c,
                                                  const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> module_name,
                                                  const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> resource,
                                                  const emulator_object<EMU_CURSORFIND> find)
        {
            if (!find)
            {
                return 0;
            }

            const auto wanted = find.read();
            const auto wanted_type = static_cast<uint16_t>(wanted.rt);
            const auto wanted_module = read_cursor_string(c, module_name);
            const auto wanted_resource = read_cursor_resource(c, resource);

            for (const auto& [index, cursor] : c.proc.cursor_icons)
            {
                if ((cursor.flags & k_cursorf_lrshared) != 0 && cursor.type == wanted_type &&
                    utils::string::equals_ignore_case(cursor.module_name, wanted_module) && cursor.resource_id == wanted_resource.id &&
                    utils::string::equals_ignore_case(cursor.resource_name, wanted_resource.name) &&
                    (wanted.cx == 0 || wanted.cx == cursor.width) && (wanted.cy == 0 || wanted.cy * 2 == cursor.height))
                {
                    return c.proc.cursor_icons.make_handle(index).bits;
                }
            }

            if (wanted.cx == 0 && wanted.cy == 0 &&
                utils::string::equals_ignore_case(wanted_module, std::u16string{k_stock_cursor_module}) && wanted_resource.name.empty() &&
                is_system_resource_id(wanted_type, wanted_resource.id))
            {
                return create_stock_cursor(c, wanted_type, wanted_resource);
            }

            return 0;
        }

        BOOL handle_NtUserDestroyCursor(const syscall_context& c, const hicon icon, const DWORD /*flags*/)
        {
            const auto* cursor = c.proc.cursor_icons.get(icon);
            if (!cursor)
            {
                set_guest_last_error(c, k_error_invalid_cursor_handle);
                return FALSE;
            }

            if ((cursor->flags & k_cursorf_lrshared) != 0)
            {
                return TRUE;
            }

            for (const auto bitmap : {cursor->mask, cursor->color})
            {
                if (bitmap != 0)
                {
                    handle_NtGdiDeleteObjectApp(c, static_cast<uint32_t>(bitmap));
                }
            }

            return c.proc.cursor_icons.erase(icon) ? TRUE : FALSE;
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

        BOOL handle_NtUserGetIconSize(const syscall_context& c, const hicon icon, const UINT /*frame*/, const emulator_object<int> cx,
                                      const emulator_object<int> cy)
        {
            const auto* cursor = c.proc.cursor_icons.get(icon);
            if (!cursor || !cx || !cy)
            {
                return FALSE;
            }

            cx.write(static_cast<int>(cursor->width));
            cy.write(static_cast<int>(cursor->height));
            return TRUE;
        }

        BOOL handle_NtUserGetIconInfo(const syscall_context& c, const hicon icon, const emulator_object<EMU_ICONINFO> icon_info,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> inst_name,
                                      const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> res_name,
                                      const emulator_object<uint32_t> bpp, const BOOL internal)
        {
            const auto* cursor = c.proc.cursor_icons.get(icon);
            if (!cursor || !icon_info)
            {
                set_guest_last_error(c, k_error_invalid_cursor_handle);
                return FALSE;
            }

            if (!is_cursor_name_writable(c, inst_name, cursor->module_name) ||
                (!(res_name && cursor->resource_name.empty()) && !is_cursor_name_writable(c, res_name, cursor->resource_name)))
            {
                return FALSE;
            }

            const auto mask_rows = cursor->color != 0 && !internal ? cursor->height / 2 : cursor->height;
            const auto mask = copy_cursor_bitmap(c, cursor->mask, mask_rows);
            const auto color = cursor->color != 0 ? copy_cursor_bitmap(c, cursor->color, cursor->height) : 0;
            if (mask == 0 || (cursor->color != 0 && color == 0))
            {
                for (const auto bitmap : {mask, color})
                {
                    if (bitmap != 0)
                    {
                        handle_NtGdiDeleteObjectApp(c, static_cast<uint32_t>(bitmap));
                    }
                }
                return FALSE;
            }

            icon_info.write(EMU_ICONINFO{
                .fIcon = cursor->type == k_cursor_type_icon,
                .xHotspot = static_cast<DWORD>(cursor->hotspot_x),
                .yHotspot = static_cast<DWORD>(cursor->hotspot_y),
                .hbmMask = mask,
                .hbmColor = color,
            });

            write_cursor_name(c, inst_name, cursor->module_name);

            if (res_name && cursor->resource_name.empty())
            {
                auto string = res_name.read();
                string.Length = 0;
                string.Buffer = cursor->resource_id;
                res_name.write(string);
            }
            else
            {
                write_cursor_name(c, res_name, cursor->resource_name);
            }

            bpp.write_if_valid(cursor->bpp);
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
            const auto cls = read_class_argument(c, class_name);
            if (cls.atom && !find_class_by_atom(c, *cls.atom))
            {
                return 0;
            }

            const auto want_class = cls.atom ? resolve_atom_name(c, *cls.atom) : cls.name;
            const auto want_name = window_name ? read_unicode_string(c.emu, window_name) : std::u16string{};
            const bool filter_class = !want_class.empty();
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

                if (filter_class &&
                    !utils::string::equals_ignore_case(std::u16string_view{candidate.class_name}, std::u16string_view{want_class}))
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

        uint64_t handle_NtUserGetProcessWindowStation(const syscall_context& c)
        {
            return c.proc.process_window_station;
        }

        bool is_window_station(const syscall_context& c, const handle station)
        {
            return station.bits == 1 || c.proc.window_stations.get(station) != nullptr;
        }

        // A window station or desktop name names a single object, so Windows refuses a backslash in it.
        bool is_valid_user_object_name(const std::u16string_view name)
        {
            return !name.empty() && name.find(u'\\') == std::u16string_view::npos;
        }

        uint64_t find_window_station(const syscall_context& c, const std::u16string_view name)
        {
            if (utils::string::equals_ignore_case(name, std::u16string_view{u"WinSta0"}))
            {
                return interactive_window_station;
            }

            for (const auto& [index, station] : c.proc.window_stations)
            {
                if (utils::string::equals_ignore_case(std::u16string_view{station.name}, name))
                {
                    return c.proc.window_stations.make_handle(index).bits;
                }
            }

            return 0;
        }

        uint64_t find_desktop(const syscall_context& c, const object_reference& window_station, const std::u16string_view name)
        {
            for (const auto& [index, desk] : c.proc.desktops)
            {
                if (!desk.opened_desktop && desk.window_station == window_station &&
                    utils::string::equals_ignore_case(std::u16string_view{desk.name}, name))
                {
                    return c.proc.desktops.make_handle(index).bits;
                }
            }

            return 0;
        }

        uint64_t handle_NtUserCreateWindowStation(const syscall_context& c,
                                                  const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                                  const ACCESS_MASK /*desired_access*/)
        {
            constexpr ULONG obj_openif = 0x80;
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_already_exists = 183;

            std::u16string name{};
            ULONG attribute_flags = 0;
            if (object_attributes)
            {
                const auto attributes = object_attributes.read();
                attribute_flags = attributes.Attributes;
                if (attributes.ObjectName)
                {
                    name = read_unicode_string(c.emu, attributes.ObjectName);
                }
            }

            // Windows names an unnamed window station after the logon session, so every unnamed create in a process
            // names the same station. 0x3e7 is the SYSTEM logon session this emulator's process runs in.
            if (name.empty())
            {
                name = u"Service-0x0-3e7$";
            }

            if (!is_valid_user_object_name(name))
            {
                set_guest_last_error(c, error_invalid_parameter);
                return 0;
            }

            // user32 asks for open-if unless CWF_CREATE_ONLY was passed, so an existing name opens that station.
            if (const auto existing = find_window_station(c, name); existing != 0)
            {
                if ((attribute_flags & obj_openif) == 0)
                {
                    set_guest_last_error(c, error_already_exists);
                    return 0;
                }

                // Each open is its own reference, so closing one handle must not take the station away from the
                // other. The interactive station is not in the store and cannot be closed.
                if (existing != interactive_window_station)
                {
                    const auto duplicated = c.proc.window_stations.duplicate(make_handle(existing));
                    if (duplicated)
                    {
                        (void)c.proc.window_stations.set_attributes(*duplicated, get_handle_attributes(object_attributes));
                    }
                    return duplicated ? duplicated->bits : 0;
                }

                return existing;
            }

            window_station station{};
            station.name = std::move(name);
            const auto handle = c.proc.window_stations.store(std::move(station));
            (void)c.proc.window_stations.set_attributes(handle, get_handle_attributes(object_attributes));
            return handle.bits;
        }

        BOOL handle_NtUserSetProcessWindowStation(const syscall_context& c, const handle station)
        {
            if (!is_window_station(c, station))
            {
                return FALSE;
            }

            c.proc.process_window_station = station.bits;
            return TRUE;
        }

        BOOL handle_NtUserCloseWindowStation(const syscall_context& c, const handle station)
        {
            // _CloseWindowStation refuses the handle the process's window station was assigned through, ignoring the two
            // tag bits user mode may set in a handle value.
            constexpr uint64_t tag_bits = 3;
            const auto untagged = station.bits & ~tag_bits;
            if (station.bits == interactive_window_station || (untagged != 0 && untagged == (c.proc.process_window_station & ~tag_bits)))
            {
                return FALSE;
            }

            return c.proc.window_stations.erase(station) ? TRUE : FALSE;
        }

        uint64_t handle_NtUserCreateDesktopEx(const syscall_context& c,
                                              const emulator_object<OBJECT_ATTRIBUTES<EmulatorTraits<Emu64>>> object_attributes,
                                              const emulator_pointer /*device*/, const emulator_pointer /*devmode*/, const DWORD /*flags*/,
                                              const ACCESS_MASK /*desired_access*/, const ULONG /*heap_size*/)
        {
            if (!object_attributes)
            {
                return 0;
            }

            const auto attributes = object_attributes.read();
            if (!attributes.ObjectName)
            {
                return 0;
            }

            auto name = read_unicode_string(c.emu, attributes.ObjectName);
            if (!is_valid_user_object_name(name))
            {
                return 0;
            }

            // CommonCreateDesktop always asks for open-if, scoped to the process window station.
            const auto station = window_station_reference(c, c.proc.process_window_station);
            if (const auto existing = find_desktop(c, station, name); existing != 0)
            {
                const auto duplicated = c.proc.desktops.duplicate(make_handle(existing));
                if (duplicated)
                {
                    (void)c.proc.desktops.set_attributes(*duplicated, get_handle_attributes(object_attributes));
                }
                return duplicated ? duplicated->bits : 0;
            }

            desktop desk{};
            desk.name = std::move(name);
            desk.window_station = station;

            const auto handle = c.proc.desktops.store(std::move(desk));
            (void)c.proc.desktops.set_attributes(handle, get_handle_attributes(object_attributes));
            return handle.bits;
        }

        hdesk handle_NtUserOpenInputDesktop(const syscall_context& c, const DWORD /*flags*/, const BOOL inherit,
                                            const ACCESS_MASK /*desired_access*/)
        {
            const auto input_desktop_handle = default_desktop(c);
            const auto* input_desktop = c.proc.desktops.get(input_desktop_handle);
            if (!input_desktop)
            {
                return 0;
            }

            desktop desk{};
            desk.name = input_desktop->name;
            desk.window_station = input_desktop->window_station;
            desk.opened_desktop = c.proc.reference_of(input_desktop_handle);
            desk.inherit = inherit != FALSE;
            return c.proc.desktops.store(std::move(desk)).bits;
        }

        BOOL handle_NtUserCloseDesktop(const syscall_context& c, const handle desktop_handle)
        {
            // A desktop that threads are using cannot be closed.
            if (c.proc.reference_of(desktop_handle) == c.proc.default_desktop)
            {
                return FALSE;
            }

            return c.proc.desktops.erase(desktop_handle) ? TRUE : FALSE;
        }

        uint16_t handle_NtUserRegisterClassExWOW(const syscall_context& c, const emulator_object<EMU_WNDCLASSEX> wnd_class_ex,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                                 const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_version,
                                                 const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name,
                                                 const DWORD function_id, const DWORD flags, const emulator_pointer /*wow*/)
        {
            if (!wnd_class_ex)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            // win32kfull's InternalRegisterClassEx sizes the class object as 0x58 + cbClsExtra in 32 bits, and a size that wraps
            // fails with ERROR_INVALID_PARAMETER. The object comes from the desktop heap, 20 MB on an interactive desktop
            // (SharedSection's second value), and the class name's atom is only added once it is allocated. An object the
            // heap cannot hold, from a more negative or a large positive extra, fails the registration without an error.
            // ponytail: the whole heap bounds one object; what the desktop's other objects already use is not counted.
            constexpr uint32_t desktop_heap_size = 20480 * 1024;
            constexpr auto class_object_size = static_cast<uint32_t>(process_context::user_class_extra_offset);
            const auto wnd_class = wnd_class_ex.read();
            const auto class_size = class_object_size + static_cast<uint32_t>(wnd_class.cbClsExtra);
            if (class_size < class_object_size)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            const auto class_name_str = read_unicode_string(c.emu, class_name);
            const auto class_version_str = class_version ? read_unicode_string(c.emu, class_version) : std::u16string{};
            const auto& class_key = class_version_str.empty() ? class_name_str : class_version_str;
            const auto existing = c.proc.classes.find(class_key);
            const auto replaces_synthesised = existing != c.proc.classes.end() && existing->second.synthesised;
            if (existing != c.proc.classes.end() && !replaces_synthesised)
            {
                // InternalRegisterClassEx first searches the process's local classes for the same hInstance, whatever the style,
                // and only after a miss searches the global classes, for a CS_GLOBALCLASS registration. The table keeps one
                // class per key, so a class of another module or scope replaces the earlier one.
                constexpr uint32_t cs_globalclass = 0x4000;
                const auto& existing_class = existing->second.wnd_class;
                const auto collides = (existing_class.style & cs_globalclass) != 0 ? (wnd_class.style & cs_globalclass) != 0
                                                                                   : existing_class.hInstance == wnd_class.hInstance;
                if (collides)
                {
                    set_guest_last_error(c, 1410); // ERROR_CLASS_ALREADY_EXISTS
                    return 0;
                }
            }

            emulator_pointer cls_ptr{};
            if (replaces_synthesised && wnd_class.cbClsExtra == 0)
            {
                cls_ptr = existing->second.guest_obj_addr;
            }
            else if (class_size <= desktop_heap_size)
            {
                cls_ptr =
                    process_context::allocate_user_class(c.win_emu.memory, class_name_str, static_cast<uint32_t>(wnd_class.cbClsExtra));
            }

            if (!cls_ptr)
            {
                return 0;
            }

            const auto index = c.proc.add_or_find_atom(class_name_str);
            if (replaces_synthesised)
            {
                const auto synthesised_object = existing->second.guest_obj_addr;
                c.proc.delete_atom(existing->second.atom);
                std::erase_if(c.proc.classes, [&](const auto& item) { return item.second.guest_obj_addr == synthesised_object; });
            }

            const auto menu_name = class_menu_name.read();
            const auto entry =
                process_context::class_entry{cls_ptr, wnd_class, menu_name, index, class_name_str, (flags & CSF_ANSIPROC) != 0};
            publish_class_to_guest(c.win_emu.memory, entry);

            // win32kfull takes the menu name from pusMenuName, not from the WNDCLASSEX, and marks only a non-empty string,
            // with -1; user32's GetClassLongPtr(GCLP_MENUNAME) then returns its client copy of the name.
            UNICODE_STRING<EmulatorTraits<Emu64>> menu_string{};
            char16_t menu_first_char{};
            const auto has_menu_string = c.win_emu.memory.try_read_memory(menu_name.pusMenuName, &menu_string, sizeof(menu_string)) &&
                                         (menu_string.Buffer >> 16) != 0 &&
                                         c.win_emu.memory.try_read_memory(menu_string.Buffer, &menu_first_char, sizeof(menu_first_char)) &&
                                         menu_first_char != 0;
            emulator_object<USER_CLASS>{c.win_emu.memory, cls_ptr}.access([&](USER_CLASS& cls) {
                cls.fnid = static_cast<uint16_t>(function_id);
                // RegisterClassExWOW passes the CSF_* flags user32 computed; CSF_ANSIPROC marks an ANSI class procedure.
                cls.CSF_flags = static_cast<uint16_t>(flags);
                cls.lpszMenuName = has_menu_string ? ~0ull : 0;
            });

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("RegisterClassEx name='" + u16_to_u8(class_name_str) + "' atom=#" +
                                                        std::to_string(index));
            }

            c.proc.classes.insert_or_assign(class_key, entry);
            c.proc.classes.insert_or_assign(make_atom_class_name(index), entry);
            if (utils::string::equals_ignore_case(std::u16string_view{class_name_str}, std::u16string_view{u"ComboLBox"}))
            {
                c.proc.classes.insert_or_assign(make_atom_class_name(k_combo_lbox_system_atom), entry);
            }

            return index;
        }

        BOOL handle_NtUserUnregisterClass(const syscall_context& c, const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                          const emulator_pointer /*instance*/,
                                          const emulator_object<CLSMENUNAME<EmulatorTraits<Emu64>>> class_menu_name)
        {
            const auto cls = read_class_argument(c, class_name);
            const auto* entry = find_registered_class(c, cls);
            if (!entry)
            {
                set_guest_last_error(c, 1411); // ERROR_CLASS_DOES_NOT_EXIST
                return FALSE;
            }

            if (class_menu_name)
            {
                class_menu_name.write(entry->menu_name);
            }

            const auto guest_obj_addr = entry->guest_obj_addr;
            const auto atom = entry->atom;
            c.win_emu.memory.release_memory(guest_obj_addr, 0);
            std::erase_if(c.proc.classes, [&](const auto& item) { return item.second.guest_obj_addr == guest_obj_addr; });
            c.proc.delete_atom(atom);
            return TRUE;
        }

        uint16_t handle_NtUserGetClassInfoEx(const syscall_context& c, const hinstance /*instance*/,
                                             const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> class_name,
                                             const emulator_object<EMU_WNDCLASSEX> wnd_class_ex, const emulator_pointer menu_name,
                                             const BOOL ansi)
        {
            const auto cls = read_class_argument(c, class_name);
            const auto* entry = find_class(c, cls);
            if (!entry)
            {
                set_guest_last_error(c, 1411); // ERROR_CLASS_DOES_NOT_EXIST

                if (c.win_emu.callbacks.on_generic_activity)
                {
                    const auto shown_name = cls.atom ? make_atom_class_name(*cls.atom) : cls.name;
                    c.win_emu.callbacks.on_generic_activity("GetClassInfoEx missing class '" + u16_to_u8(shown_name) + "'");
                }

                return 0;
            }

            if (wnd_class_ex)
            {
                auto result = entry->wnd_class;
                result.lpszClassName = 0;
                result.lpszMenuName = 0;
                wnd_class_ex.write(result);
            }

            if (menu_name)
            {
                const auto value = ansi ? entry->menu_name.pszClientAnsiMenuName : entry->menu_name.pwszClientUnicodeMenuName;
                emulator_object<uint64_t>{c.emu, menu_name}.write(value);
            }

            return entry->atom;
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

        std::optional<uint64_t> allocate_hook_handle(const syscall_context& c)
        {
            std::set<uint64_t> used_indices{};
            for (const auto& thread : c.proc.threads | std::views::values)
            {
                for (const auto& hook : thread.thread_hooks)
                {
                    used_indices.insert(hook.handle & 0xFFFF);
                }

                for (const auto& hook : thread.low_level_hooks)
                {
                    used_indices.insert(hook.handle & 0xFFFF);
                }
            }

            uint64_t index = 1;
            while (used_indices.contains(index))
            {
                ++index;
            }

            if (index > k_hook_index_mask)
            {
                set_guest_last_error(c, k_error_not_enough_memory);
                return std::nullopt;
            }

            const uint64_t uniqueness = c.proc.next_hook_uniqueness;
            c.proc.next_hook_uniqueness = uniqueness < k_hook_uniqueness_last ? static_cast<uint32_t>(uniqueness + 1) : 1;
            return (uniqueness << 16) | index;
        }

        bool hook_handle_matches(const syscall_context& c, const uint64_t entry, const uint64_t requested)
        {
            if ((entry & k_hook_index_mask) != (requested & k_hook_index_mask))
            {
                return false;
            }

            const auto requested_uniqueness = (requested >> 16) & k_hook_uniqueness_mask;
            return requested_uniqueness == k_hook_uniqueness_mask || requested_uniqueness == ((entry >> 16) & k_hook_uniqueness_mask) ||
                   (c.proc.is_wow64_process && requested_uniqueness == 0);
        }

        uint64_t install_window_hook(const syscall_context& c, const hinstance instance, const DWORD thread_id, const int hook_id,
                                     const pointer proc, const BOOL ansi)
        {
            if (hook_id < k_wh_msgfilter || hook_id > k_wh_mouse_ll)
            {
                set_guest_last_error(c, k_error_invalid_hook_filter);
                return 0;
            }

            if (proc == 0)
            {
                set_guest_last_error(c, k_error_invalid_filter_proc);
                return 0;
            }

            if (hook_id == k_wh_journalrecord || hook_id == k_wh_journalplayback)
            {
                return 0;
            }

            const bool low_level = hook_id == k_wh_keyboard_ll || hook_id == k_wh_mouse_ll;
            if ((low_level || hook_id == k_wh_sysmsgfilter) && thread_id != 0)
            {
                set_guest_last_error(c, k_error_global_only_hook);
                return 0;
            }

            auto& thread = c.thread();

            // Low-level hooks are global; they run on the installing thread while it retrieves messages.
            if (low_level)
            {
                const auto handle = allocate_hook_handle(c);
                if (!handle)
                {
                    return 0;
                }

                thread.low_level_hooks.push_back(user_low_level_hook{.handle = *handle, .id = hook_id, .proc = proc});
                return *handle;
            }

            if (thread_id == 0)
            {
                if (instance == 0)
                {
                    set_guest_last_error(c, k_error_hook_needs_hmod);
                }
                return 0;
            }

            auto* target = c.proc.find_thread_by_id(thread_id);
            const bool is_gui_thread = target && (target == &thread || target->win32k_thread_info != 0);
            if (!is_gui_thread || target->is_terminated())
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return 0;
            }

            const auto handle = allocate_hook_handle(c);
            if (!handle)
            {
                return 0;
            }

            target->thread_hooks.push_back(user_thread_hook{
                .handle = *handle,
                .id = hook_id,
                .proc = proc,
                .ansi = ansi != FALSE,
                .installer = thread.id,
                .destroyed = false,
            });
            target->publish_hook_flags();
            return *handle;
        }

        uint64_t handle_NtUserSetWindowsHookEx(const syscall_context& c, const hinstance instance,
                                               const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> /*module*/,
                                               const DWORD thread_id, const int hook_id, const pointer proc, const BOOL ansi)
        {
            return install_window_hook(c, instance, thread_id, hook_id, proc, ansi);
        }

        uint64_t handle_NtUserSetWindowsHookAW(const syscall_context& c, const int hook_id, const pointer proc, const BOOL ansi)
        {
            auto& thread = c.thread();
            const bool first_of_type = !thread.chains_hooks(hook_id);
            const auto handle = install_window_hook(c, 0, thread.id, hook_id, proc, ansi);
            if (handle == 0)
            {
                return std::numeric_limits<uint64_t>::max();
            }

            // The hook type's table in win32kfull flags (0x8) whether its first install returns the HHOOK.
            const bool type_returns_handle_on_first_install = hook_id == k_wh_msgfilter || hook_id == k_wh_keyboard;
            return first_of_type && !type_returns_handle_on_first_install ? 0 : handle;
        }

        BOOL handle_NtUserUnhookWindowsHookEx(const syscall_context& c, const uint64_t hook)
        {
            const auto requested = hook & 0xFFFFFFFF;
            for (auto& thread : c.proc.threads | std::views::values)
            {
                if (std::erase_if(thread.low_level_hooks,
                                  [&](const user_low_level_hook& entry) { return hook_handle_matches(c, entry.handle, requested); }) != 0)
                {
                    return TRUE;
                }

                const auto found = std::ranges::find_if(thread.thread_hooks, [&](const user_thread_hook& entry) {
                    return !entry.destroyed && hook_handle_matches(c, entry.handle, requested);
                });
                if (found != thread.thread_hooks.end())
                {
                    thread.destroy_hook(found->handle);
                    return !thread.is_terminated();
                }
            }

            set_guest_last_error(c, k_error_invalid_hook_handle);
            return FALSE;
        }

        BOOL handle_NtUserUnhookWindowsHook(const syscall_context& c, const int hook_id, const pointer proc)
        {
            if (hook_id < k_wh_msgfilter || hook_id > k_wh_mouse_ll)
            {
                set_guest_last_error(c, k_error_invalid_hook_filter);
                return FALSE;
            }

            auto& thread = c.thread();
            for (const auto& hook : thread.thread_hooks | std::views::reverse)
            {
                if (hook.id == hook_id && hook.proc == proc && !hook.destroyed)
                {
                    if (hook.installer != thread.id)
                    {
                        set_guest_last_error(c, k_error_access_denied);
                        return FALSE;
                    }

                    thread.destroy_hook(hook.handle);
                    return TRUE;
                }
            }

            set_guest_last_error(c, k_error_hook_not_installed);
            return FALSE;
        }

        template <typename T>
        bool read_guest(const syscall_context& c, const uint64_t address, T& value, const size_t size = sizeof(T))
        {
            if (!is_readable_range(c.win_emu, address, size))
            {
                return false;
            }

            c.win_emu.memory.read_memory(address, &value, size);
            return true;
        }

        template <typename Element>
        bool read_hook_text(const syscall_context& c, const pointer address, std::basic_string<Element>& text)
        {
            for (size_t i = 0; i < k_max_hook_text_length; ++i)
            {
                Element unit{};
                if (!read_guest(c, address + (i * sizeof(Element)), unit))
                {
                    return false;
                }

                if (unit == 0)
                {
                    break;
                }

                text.push_back(unit);
            }

            return true;
        }

        bool convert_hook_text(const syscall_context& c, pointer& address, const bool is_atom_allowed, const bool from_ansi,
                               const bool to_ansi, std::vector<uint64_t>& allocations)
        {
            if (address == 0 || (is_atom_allowed && address <= std::numeric_limits<uint16_t>::max()))
            {
                return true;
            }

            std::string narrow{};
            std::u16string wide{};
            if (from_ansi ? !read_hook_text(c, address, narrow) : !read_hook_text(c, address, wide))
            {
                return false;
            }

            if (from_ansi == to_ansi)
            {
                return true;
            }

            if (from_ansi)
            {
                wide = cp1252_to_u16(narrow);
            }
            else
            {
                narrow = u16_to_cp1252(wide);
            }

            const void* data = to_ansi ? static_cast<const void*>(narrow.c_str()) : wide.c_str();
            const auto size = to_ansi ? narrow.size() + 1 : (wide.size() + 1) * sizeof(char16_t);
            address = c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(size)), memory_permission::read_write);
            c.win_emu.memory.write_memory(address, data, size);
            allocations.push_back(address);
            return true;
        }

        void release_hook_text(const syscall_context& c, hook_call_state& state)
        {
            for (const auto address : state.converted_text)
            {
                c.win_emu.memory.release_memory(address, 0);
            }

            state.converted_text.clear();
        }

        bool dispatch_next_cbt_create_hook(const syscall_context& c, const user_thread_hook& next, const wparam w_param,
                                           const lparam l_param, const bool caller_ansi)
        {
            const auto* win = c.proc.windows.get(w_param);
            if (!win || c.proc.is_wow64_process)
            {
                return false;
            }

            struct cbt_create_wnd
            {
                pointer lpcs{};
                hwnd hwndInsertAfter{};
            };

            cbt_create_wnd create{};
            fn_hk_in_lp_cbt_create_struct_message args{};
            if (!read_guest(c, l_param, create) || !is_writable_range(c.win_emu, create.lpcs, sizeof(args.cs)) ||
                !read_guest(c, create.lpcs, args.cs))
            {
                return false;
            }

            hook_call_state state{};
            state.hook_id = k_wh_cbt;
            if (!convert_hook_text(c, args.cs.lpszName, false, caller_ansi, next.ansi, state.converted_text) ||
                !convert_hook_text(c, args.cs.lpszClass, true, caller_ansi, next.ansi, state.converted_text))
            {
                release_hook_text(c, state);
                return false;
            }

            args.pwnd = win->guest.value();
            args.msg = static_cast<UINT>(k_hcbt_createwnd | (k_wh_cbt << 16));
            args.wParam = w_param;
            args.hwndInsertAfter = create.hwndInsertAfter;
            args.xpfnProc = next.proc;
            args.ansi = next.ansi ? TRUE : FALSE;

            enter_hook(c, next);
            dispatch_user_callback(c, callback_id::NtUserCallNextHookEx, k_fn_hk_in_lp_cbt_create_struct_callback_id, std::move(state),
                                   args);
            return true;
        }

        lresult handle_NtUserCallNextHookEx(const syscall_context& c, const int code, const wparam w_param, const lparam l_param,
                                            const BOOL ansi)
        {
            auto& thread = c.thread();
            if (thread.running_hooks.empty())
            {
                return 0;
            }

            const auto* running = thread.find_hook(thread.running_hooks.back().handle);
            const auto* next = running ? thread.next_older_hook(*running) : nullptr;
            if (!next)
            {
                return 0;
            }

            if (next->id == k_wh_getmessage || next->id == k_wh_msgfilter)
            {
                const bool writes_back = next->id == k_wh_getmessage;
                msg message{};
                if (!read_guest(c, l_param, message) || (writes_back && !is_writable_range(c.win_emu, l_param, sizeof(message))))
                {
                    return 0;
                }

                dispatch_message_hook(c, callback_id::NtUserCallNextHookEx, *next, code, w_param, message);
                return {};
            }

            if (next->id == k_wh_cbt && code == k_hcbt_createwnd &&
                dispatch_next_cbt_create_hook(c, *next, w_param, l_param, ansi != FALSE))
            {
                return {};
            }

            return 0;
        }

        lresult completion_NtUserCallNextHookEx(const syscall_context& c, const int /*code*/, const wparam /*w_param*/,
                                                const lparam l_param, const BOOL /*ansi*/)
        {
            auto& state = c.get_completion_state<hook_call_state>();
            if (state.hook_id == k_wh_cbt)
            {
                leave_hook(c);
                release_hook_text(c, state);
            }
            else if (const auto hooked = complete_message_hook(c); hooked && state.hook_id == k_wh_getmessage)
            {
                if (!is_writable_range(c.win_emu, l_param, sizeof(*hooked)))
                {
                    return 0;
                }

                c.win_emu.memory.write_memory(l_param, &*hooked, sizeof(*hooked));
            }

            return c.get_callback_result<lresult>();
        }

        BOOL handle_NtUserCallMsgFilter(const syscall_context& c, const emulator_object<msg> message, const int code)
        {
            const auto* hook = c.thread().newest_hook(k_wh_msgfilter);
            if (!hook || !is_readable_range(c.win_emu, message.value(), sizeof(msg)))
            {
                return FALSE;
            }

            dispatch_message_hook(c, callback_id::NtUserCallMsgFilter, *hook, code, 0, message.read());
            return {};
        }

        BOOL completion_NtUserCallMsgFilter(const syscall_context& c, const emulator_object<msg> /*message*/, const int /*code*/)
        {
            complete_message_hook(c);
            return c.get_callback_result<lresult>() != 0 ? TRUE : FALSE;
        }

        // Windows clips every window that is not a child against its siblings and gives every overlapped one a caption.
        DWORD created_window_style(DWORD style)
        {
            if ((style & (WS_CHILD | WS_POPUP)) != WS_CHILD)
            {
                style |= WS_CLIPSIBLINGS;
                if ((style & WS_POPUP) == 0)
                {
                    style |= WS_CAPTION;
                }
            }
            return style;
        }

        hwnd handle_NtUserCreateWindowEx(const syscall_context& c, const DWORD ex_style, const emulator_object<LARGE_STRING> class_name,
                                         const emulator_object<LARGE_STRING> class_version, const emulator_object<LARGE_STRING> window_name,
                                         const DWORD requested_style, int x, int y, int width, int height, const hwnd parent,
                                         const hmenu menu, const hinstance instance, const pointer l_param, const DWORD /*flags*/,
                                         const pointer /*acbi_buffer*/)
        {
            const auto style = created_window_style(requested_style);
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
                auto style_string = utils::string::to_hex_number(requested_style);
                style_string.insert(style_string.begin(), std::max(0, 8 - static_cast<int>(style_string.size())), '0');
                c.win_emu.callbacks.on_generic_activity("CreateWindowEx class='" + u16_to_u8(cls_name) + "' style=0x" + style_string);
            }

            const auto cls_version = read_large_string_or_atom(c, class_version);
            const auto& cls_key = cls_version.empty() ? cls_name : cls_version;
            auto cls_it = c.proc.classes.find(cls_key);

            if (cls_it == c.proc.classes.end())
            {
                if (const auto* builtin = ensure_builtin_window_class(c, cls_key))
                {
                    cls_it = c.proc.classes.find(cls_key);
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
            win.message_only = is_message_only;
            // Record the owning thread in the shared handle entry so client-side GetWindowThreadProcessId works.
            c.proc.user_handles.set_owner(static_cast<uint32_t>(handle.value.id), win.thread_id);
            if (!is_message_only)
            {
                win.parent_handle = has_child_parent ? parent : c.proc.default_desktop_window_handle.bits;
                win.owner_handle = has_owner ? parent : 0;
            }
            win.class_name = cls_it->second.name;
            win.class_key = cls_key;
            const auto normalized_class = normalize_builtin_window_class_name(win.class_name);
            // Builtin controls use the wide (apfnClientW) wndproc (expect UTF-16); app classes keep
            // their registered proc. Drives the WM_SETTEXT re-encoding so a wide proc never gets ANSI.
            win.unicode_proc = is_builtin_window_class_name(normalized_class) || !cls_it->second.ansi_proc;
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

            if (c.win_emu.callbacks.on_generic_activity && !win.name.empty())
            {
                c.win_emu.callbacks.on_generic_activity("CreateWindowEx class='" + u16_to_u8(cls_name) + "' name='" + u16_to_u8(win.name) +
                                                        "'");
            }
            win.wnd_proc = wnd_class->lpfnWndProc;

            win.guest.access([&](USER_WINDOW& guest_win) {
                guest_win.hWnd = handle.bits;
                guest_win.ptrBase = win.guest.value();
                guest_win.dwExStyle = ex_style;
                guest_win.dwStyle = style;
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
                if (!win.unicode_proc)
                {
                    guest_win.bFlags |= USER_WINDOW_ANSI_WINDOW_PROC;
                }
                guest_win.pcls = class_obj_addr;
                guest_win.hrgnUpdate = !is_message_only ? k_hrgn_window : 0;
                guest_win.cbWndExtra = wnd_class->cbWndExtra;
                // Control id offset is build-specific: Win11 reads wID (WND+0x140), Server 2022 reads
                // spmenu (WND+0x98). Populate both so builtin wndprocs emit the right WM_COMMAND id.
                guest_win.wID = has_child_parent ? menu : 0;
                if (has_child_parent)
                {
                    guest_win.spmenu = menu;
                }
                guest_win.windowBand = 1; // ZBID_DESKTOP
                guest_win.dpiContext = USER_DEFAULT_WINDOW_DPI_CONTEXT;
                // user32's window procedures set the FNID themselves and refuse a window that carries another one, e.g. the
                // list box procedure of a ComboLBox window rejects FNID_COMBOLBOX, so only a synthesised class presets it.
                if (cls_it->second.synthesised)
                {
                    guest_win.fnid = emulator_object<USER_CLASS>{c.win_emu.memory, class_obj_addr}.read().fnid;
                }
                guest_win.threadId = win.thread_id;
                guest_win.processId = c.proc.process_id;

                win.host_surface_window = !is_message_only;

                if (wnd_class->cbWndExtra > 0)
                {
                    const auto extra_size = static_cast<size_t>(page_align_up(wnd_class->cbWndExtra));
                    guest_win.pExtraBytes = c.win_emu.memory.allocate_memory(extra_size, memory_permission::read_write);
                }
            });
            c.proc.sync_guest_window_rects(win);
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
                const bool hosted_child = parent_win && !c.proc.is_surface_root(win);
                c.win_emu.ui().create_window(ui_window_desc{
                    .handle = handle.bits,
                    .parent = hosted_child ? parent_win->handle : 0,
                    .owner = has_owner && parent_win ? parent_win->handle : 0,
                    .rect = get_window_rect(win),
                    .client_insets = get_host_ui_client_insets(win),
                    .class_name = std::u16string{normalize_builtin_window_class_name(win.class_name)},
                    .title = win.name,
                    .style = style,
                    .ex_style = ex_style,
                    .control_id = hosted_child ? static_cast<uint32_t>(menu) : 0,
                    .visible = (style & WS_VISIBLE) != 0,
                    .enabled = (style & WS_DISABLED) == 0,
                    .top_level = !hosted_child,
                });

                if (has_child_parent)
                {
                    if (parent_win && (style & WS_VISIBLE) != 0)
                    {
                        invalidate_window(c, win);
                    }
                }
                else
                {
                    sogen::msg queued_message{};
                    queued_message.window = handle.bits;
                    queued_message.message = WM_DWMNCRENDERINGCHANGED;
                    queued_message.wParam = 1;
                    queued_message.lParam = 0;
                    c.vcpu.active_thread->post_message(c.win_emu, queued_message);
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
            state.parent_handle = has_child_parent && parent_win ? parent_win->handle : 0;

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
            const auto creator_name = cs.lpszName;
            const auto creator_class = cs.lpszClass;
            const auto text_in_encoding = [&](const emulator_object<LARGE_STRING> text, const pointer creator_text, const bool ansi) {
                const auto converted = convert_create_struct_text(c, text, ansi);
                if (!converted)
                {
                    return creator_text;
                }

                state.converted_create_struct_text.push_back(*converted);
                return static_cast<pointer>(*converted);
            };
            cs.lpszName = text_in_encoding(window_name, creator_name, !win.unicode_proc);
            cs.lpszClass = text_in_encoding(class_name, creator_class, !win.unicode_proc);
            state.create_struct_alloc = c.emu.push_stack(cs);

            RECT wr{};
            state.window_rect_alloc = c.emu.push_stack(wr);

            EMU_MINMAXINFO mmi{};
            state.min_max_info_alloc = c.emu.push_stack(mmi);

            state.message_queue = {
                {.message = WM_CREATE, .wParam = 0, .lParam = state.create_struct_alloc.address()},
                {.message = WM_NCCALCSIZE, .wParam = 0, .lParam = state.window_rect_alloc.address()},
                {.message = WM_NCCREATE, .wParam = 0, .lParam = state.create_struct_alloc.address()},
            };
            const bool notify_parent = has_child_parent && (ex_style & WS_EX_NOPARENTNOTIFY) == 0;
            const auto parent_notify_wparam = static_cast<uint64_t>(WM_CREATE) | (static_cast<uint64_t>(menu & 0xFFFF) << 16);
            if (!has_child_parent)
            {
                state.message_queue.push_back({.message = WM_GETMINMAXINFO, .wParam = 0, .lParam = state.min_max_info_alloc.address()});
            }

            const bool initially_visible = (style & WS_VISIBLE) != 0;
            if (has_child_parent)
            {
                const auto move_lparam = pack_message_lparam(win.client_x(), win.client_y());
                const auto size_lparam = pack_message_lparam(win.client_width(), win.client_height());
                std::vector<qmsg> child_messages{};

                if (initially_visible)
                {
                    invalidate_window(c, win);
                    child_messages.push_back({.message = WM_SHOWWINDOW, .wParam = TRUE, .lParam = 0});
                }
                if (notify_parent)
                {
                    child_messages.push_back({.message = WM_PARENTNOTIFY, .wParam = parent_notify_wparam, .lParam = handle.bits});
                }
                child_messages.push_back({.message = WM_MOVE, .wParam = 0, .lParam = move_lparam});
                child_messages.push_back({.message = WM_SIZE, .wParam = 0, .lParam = size_lparam});
                state.message_queue.insert(state.message_queue.begin(), child_messages.begin(), child_messages.end());
            }
            else if (initially_visible)
            {
                invalidate_window(c, win);
                window_show_orchestrator{state.show_data, c}.start_show(win, SWP_SHOWWINDOW, true, true,
                                                                        !win.message_only && !is_application_active(c));
            }

            if (c.win_emu.callbacks.on_generic_activity)
            {
                c.win_emu.callbacks.on_generic_activity("CreateWindowEx async class='" + u16_to_u8(cls_name) + "' hwnd=0x" +
                                                        utils::string::to_hex_number(handle.bits));
            }

            if (const auto* hook = c.thread().newest_hook(k_wh_cbt))
            {
                const auto hook_name = text_in_encoding(window_name, creator_name, hook->ansi);
                const auto hook_class = text_in_encoding(class_name, creator_class, hook->ansi);
                state.phase = window_create_phase::cbt_create;
                dispatch_cbt_create_window(c, std::move(state), win, *hook, hook_name, hook_class);
                return {};
            }

            state.phase = window_create_phase::creation_messages;
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
            auto* win = c.proc.windows.get(s.handle);
            auto show_orchestrator = window_show_orchestrator{s.show_data, c};

            if (s.phase == window_create_phase::cbt_create)
            {
                leave_hook(c);
            }

            const auto release_window_create_allocations = [&] {
                show_orchestrator.release();

                c.emu.pop_stack(s.min_max_info_alloc);
                c.emu.pop_stack(s.window_rect_alloc);
                c.emu.pop_stack(s.create_struct_alloc);
                release_converted_create_struct_text(c, s);
            };

            if (!win)
            {
                release_window_create_allocations();
                return 0;
            }

            if (s.phase == window_create_phase::cbt_create)
            {
                if (c.get_callback_result<lresult>() != 0)
                {
                    release_window_create_allocations();
                    c.win_emu.ui().destroy_window(win->handle);
                    c.proc.release_mouse_capture(win->handle);
                    c.proc.release_update_lock(win->handle);
                    c.proc.gdi_window_surfaces.erase(static_cast<uint32_t>(win->handle));
                    (void)c.proc.windows.erase(s.handle);
                    return 0;
                }

                s.phase = window_create_phase::creation_messages;
                dispatch_next_message(c, callback_id::NtUserCreateWindowEx, std::move(s), *win, s.message_queue);
                return {};
            }

            show_orchestrator.erase_background_completed(c.get_callback_result<uint64_t>());

            if (s.show_data.pending_window_pos_address != 0)
            {
                const bool activation_position =
                    s.show_data.activation_window_pos_alloc &&
                    s.show_data.pending_window_pos_address == s.show_data.activation_window_pos_alloc.address();

                const auto position = resolve_window_position_callback(c, s.show_data.pending_window_pos_address);
                apply_window_position_change(c, *win, position, s.show_data.changed_window_pos_alloc, s.show_data.message_queue,
                                             !activation_position);

                s.show_data.pending_window_pos_address = 0;
                show_orchestrator.window_position_completed(activation_position);
            }

            if (!s.message_queue.empty())
            {
                const auto& next = s.message_queue.back();
                if (next.message == WM_PARENTNOTIFY)
                {
                    const auto* parent_win = c.proc.windows.get(s.parent_handle);
                    if (parent_win)
                    {
                        dispatch_next_message(c, callback_id::NtUserCreateWindowEx, std::move(s), *parent_win, s.message_queue);
                        return {};
                    }
                    s.message_queue.pop_back();
                }
                else if (next.message == WM_WINDOWPOSCHANGING)
                {
                    s.show_data.pending_window_pos_address = next.lParam;
                }

                dispatch_next_message(c, callback_id::NtUserCreateWindowEx, std::move(s), *win, s.message_queue);
                return {};
            }

            while (const auto step = show_orchestrator.advance())
            {
                if (auto* target = c.proc.windows.get(step->handle))
                {
                    dispatch_window_message(c, callback_id::NtUserCreateWindowEx, std::move(s), *target, step->message.message,
                                            step->message.wParam, step->message.lParam);
                    return {};
                }
            }

            c.emu.pop_stack(s.min_max_info_alloc);
            c.emu.pop_stack(s.window_rect_alloc);
            c.emu.pop_stack(s.create_struct_alloc);
            release_converted_create_struct_text(c, s);

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
            window_destroy_orchestrator{state.destruction, c}.start(*win);
            return advance_window_destroy(c, state, state.destruction, callback_id::NtUserDestroyWindow);
        }

        BOOL completion_NtUserDestroyWindow(const syscall_context& c, const hwnd /*window*/)
        {
            auto& s = c.get_completion_state<window_destroy_state>();
            complete_window_destroy_step(c, s.destruction);
            return advance_window_destroy(c, s, s.destruction, callback_id::NtUserDestroyWindow);
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

        BOOL handle_NtUserChangeWindowMessageFilterEx()
        {
            return TRUE;
        }

        BOOL handle_NtUserChangeWindowMessageFilter()
        {
            return TRUE;
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
            const bool is_child = (win->style & WS_CHILD) != 0 && (win->style & WS_POPUP) == 0;
            const bool command_allows_activation = cmd_show != SW_SHOWNOACTIVATE && cmd_show != SW_SHOWMINNOACTIVE && cmd_show != SW_SHOWNA;
            const bool activate_window = command_allows_activation && !is_child;

            if (want_visible == was_visible)
            {
                return was_visible ? TRUE : FALSE;
            }

            window_show_state state{};
            state.was_visible = was_visible;
            auto orchestrator = window_show_orchestrator{state.data, c};

            uint32_t visibility_flags{};
            if (want_visible)
            {
                visibility_flags = SWP_SHOWWINDOW;
                if (is_child || !command_allows_activation)
                {
                    visibility_flags |= SWP_NOACTIVATE;
                }
                if (is_child)
                {
                    visibility_flags |= SWP_NOZORDER;
                }
            }
            else
            {
                visibility_flags = SWP_HIDEWINDOW | SWP_NOZORDER | SWP_NOACTIVATE;
            }

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
                orchestrator.start_show(*win, visibility_flags, activate_window, !is_child,
                                        activate_window && !win->message_only && !is_application_active(c));

                if (is_child && win->parent_handle != 0)
                {
                    // A combo box moves its drop-down list under the desktop window, whose window procedure lives outside
                    // the process, so the process can neither repaint nor erase it. Any other parent is repainted, and
                    // erased right away only when its procedure runs on this thread.
                    if (auto* parent_win = c.proc.windows.get(win->parent_handle); parent_win && parent_win->wnd_proc != 0)
                    {
                        invalidate_window(c, *parent_win, std::nullopt, true);
                        if (parent_win->thread_id == c.thread().id)
                        {
                            orchestrator.set_parent_erase_window(parent_win->handle);
                        }
                    }
                }

                win->style |= WS_VISIBLE;
            }
            else
            {
                orchestrator.start_hide(*win, visibility_flags);
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

            return advance_window_show(c, state);
        }

        BOOL completion_NtUserShowWindow(const syscall_context& c, const hwnd /*hwnd*/, const LONG /*cmd_show*/)
        {
            auto& s = c.get_completion_state<window_show_state>();
            auto& data = s.data;
            auto orchestrator = window_show_orchestrator{data, c};

            orchestrator.erase_background_completed(c.get_callback_result<uint64_t>());

            if (data.pending_window_pos_address != 0)
            {
                const bool activation_position =
                    data.activation_window_pos_alloc && data.pending_window_pos_address == data.activation_window_pos_alloc.address();
                if (auto* win = c.proc.windows.get(data.handle))
                {
                    const auto position = resolve_window_position_callback(c, data.pending_window_pos_address);
                    apply_window_position_change(c, *win, position, data.changed_window_pos_alloc, data.message_queue,
                                                 !activation_position);
                }

                data.pending_window_pos_address = 0;
                orchestrator.window_position_completed(activation_position);
            }

            return advance_window_show(c, s);
        }

        uint64_t prepare_message_text(const syscall_context& c, window& win, const UINT msg, const uint64_t w_param, const uint64_t l_param,
                                      const BOOL ansi)
        {
            uint64_t scratch_text = 0;
            const bool caller_unicode = ansi == FALSE;

            if (msg == WM_SETTEXT)
            {
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

                // Deliver the text in the target proc's encoding (caller's is `ansi`, proc's is
                // unicode_proc): when they differ, re-encode into a scratch guest buffer so a wide proc
                // never reads ANSI as UTF-16. Freed after the window procedure returns.
                if (l_param != 0 && caller_unicode != win.unicode_proc)
                {
                    if (win.unicode_proc)
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
                }
            }
            else if (msg == WM_GETTEXT && l_param != 0 && w_param != 0 && caller_unicode != win.unicode_proc)
            {
                const auto proc_char_size = win.unicode_proc ? sizeof(char16_t) : sizeof(char);
                const auto bytes = (w_param + 1) * proc_char_size;
                scratch_text = c.win_emu.memory.allocate_memory(static_cast<size_t>(page_align_up(bytes)), memory_permission::read_write);
            }

            return scratch_text;
        }

        std::optional<uint64_t> copy_message_text_result(const syscall_context& c, const UINT msg, const uint64_t w_param,
                                                         const uint64_t l_param, const BOOL ansi, const uint64_t scratch_text)
        {
            if (msg != WM_GETTEXT || scratch_text == 0)
            {
                return std::nullopt;
            }
            const auto capacity = static_cast<size_t>(std::min<uint64_t>(w_param, std::numeric_limits<size_t>::max()));
            auto text = ansi ? read_string<char16_t>(c.win_emu.memory, scratch_text, capacity)
                             : cp1252_to_u16(read_string<char>(c.win_emu.memory, scratch_text, capacity));
            text.erase(std::min(text.find(u'\0'), text.size()));
            return copy_text_to_buffer(c, text, w_param, l_param, ansi != FALSE);
        }

        void complete_sent_window_message(const syscall_context& c, sent_window_message& message, const uint64_t result)
        {
            if (c.proc.reply_to_sent_message(message, result))
            {
                c.win_emu.wake_idle_vcpu();
            }
        }

        bool dispatch_pending_sent_window_message(const syscall_context& c, const callback_id completion_callback)
        {
            auto& thread = c.thread();
            while (!thread.pending_sent_messages.empty())
            {
                auto message = thread.pending_sent_messages.front();
                thread.pending_sent_messages.erase(thread.pending_sent_messages.begin());
                thread.publish_client_thread_info();
                auto* win = c.proc.windows.get(message.window);
                if (!win || win->thread_id != thread.id)
                {
                    complete_sent_window_message(c, message, 0);
                    continue;
                }
                message.scratch_text = prepare_message_text(c, *win, message.message, message.w_param, message.l_param, message.ansi);
                thread.active_sent_messages.push_back(message);
                thread.publish_client_thread_info();
                const auto param = message.scratch_text != 0 ? message.scratch_text : message.l_param;
                if (completion_callback == callback_id::NtUserMessageCall)
                {
                    message_call_state state{};
                    state.dispatching_nested_send = true;
                    dispatch_window_message(c, completion_callback, std::move(state), *win, message.message, message.w_param, param);
                }
                else
                {
                    message_retrieval_state state{};
                    state.callback = message_retrieval_callback::sent_message;
                    dispatch_window_message(c, completion_callback, std::move(state), *win, message.message, message.w_param, param);
                }
                return true;
            }
            return false;
        }

        void complete_active_sent_window_message(const syscall_context& c)
        {
            auto& messages = c.thread().active_sent_messages;
            if (messages.empty())
            {
                throw std::runtime_error("Missing active sent window message");
            }
            auto message = messages.back();
            messages.pop_back();
            c.thread().publish_client_thread_info();
            if (!message.replied)
            {
                const auto converted =
                    copy_message_text_result(c, message.message, message.w_param, message.l_param, message.ansi, message.scratch_text);
                complete_sent_window_message(c, message, converted.value_or(c.get_callback_result<uint64_t>()));
            }
            if (message.scratch_text != 0)
            {
                c.win_emu.memory.release_memory(message.scratch_text, 0);
            }
        }

        uint64_t continue_sent_message_wait(const syscall_context& c, const sent_message_wait& wait)
        {
            if (wait.result)
            {
                const auto result = *wait.result;
                c.thread().sent_message_waits.pop_back();
                return result;
            }
            if (!dispatch_pending_sent_window_message(c, callback_id::NtUserMessageCall))
            {
                c.win_emu.yield_thread(c.vcpu, false);
            }
            return 0;
        }

        uint64_t handle_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t w_param,
                                          const uint64_t l_param, const uint64_t result_info, const DWORD type, const BOOL ansi)
        {
            if (const auto* wait = c.thread().current_sent_message_wait())
            {
                return continue_sent_message_wait(c, *wait);
            }
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            if (type == FNID_DEFWINDOW)
            {
                if (msg == WM_CLOSE)
                {
                    message_call_state state{};
                    state.window = hwnd;
                    state.message = msg;
                    state.destroying_window = true;
                    window_destroy_orchestrator{state.destruction, c}.start(*win);
                    return advance_window_destroy(c, state, state.destruction, callback_id::NtUserMessageCall);
                }

                const auto result = handle_default_window_proc_message(c, *win, msg, w_param, l_param, ansi);
                return write_message_call_result(c, result_info, result) ? TRUE : FALSE;
            }

            if (win->wnd_proc == 0)
            {
                return handle_default_window_proc_message(c, *win, msg, w_param, l_param, ansi);
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

                if (type == FNID_SENDMESSAGE || type == FNID_SENDMESSAGEFF)
                {
                    if (auto* target = c.proc.find_thread_by_id(win->thread_id); target && !target->is_terminated())
                    {
                        const auto depth = c.thread().callback_stack.size();
                        const auto& abi = c.win_emu.abi();
                        c.thread().sent_message_waits.push_back({.callback_depth = depth,
                                                                 .syscall_ip = abi.instruction_pointer(c.emu),
                                                                 .syscall_number = abi.return_value(c.emu)});
                        target->pending_sent_messages.push_back({.sender_thread_id = c.thread().id,
                                                                 .sender_callback_depth = depth,
                                                                 .window = hwnd,
                                                                 .message = msg,
                                                                 .w_param = w_param,
                                                                 .l_param = l_param,
                                                                 .ansi = ansi});
                        target->mark_queue_status_changed(QS_SENDMESSAGE);
                        c.win_emu.wake_idle_vcpu();
                        c.win_emu.yield_thread(c.vcpu, false);
                    }
                }
                return 0;
            }

            const auto scratch_text = prepare_message_text(c, *win, msg, w_param, l_param, ansi);
            message_call_state state{};
            state.window = hwnd;
            state.message = msg;
            state.scratch_text = scratch_text;
            dispatch_window_message(c, callback_id::NtUserMessageCall, std::move(state), *win, msg, w_param,
                                    scratch_text != 0 ? scratch_text : l_param);
            return {};
        }

        uint64_t completion_NtUserMessageCall(const syscall_context& c, const hwnd hwnd, const UINT msg, const uint64_t w_param,
                                              const uint64_t l_param, const uint64_t result_info, const DWORD type, const BOOL ansi)
        {
            auto& state = c.get_completion_state<message_call_state>();

            if (state.dispatching_nested_send)
            {
                complete_active_sent_window_message(c);
                return continue_sent_message_wait(c, *c.thread().current_sent_message_wait());
            }

            if (state.destroying_window)
            {
                complete_window_destroy_step(c, state.destruction);
                if (!advance_window_destroy(c, state, state.destruction, callback_id::NtUserMessageCall))
                {
                    return {};
                }
                return write_message_call_result(c, result_info, 0) ? TRUE : FALSE;
            }

            const auto converted_text_length = copy_message_text_result(c, state.message, w_param, l_param, ansi, state.scratch_text);
            if (state.scratch_text != 0)
            {
                c.win_emu.memory.release_memory(state.scratch_text, 0);
                state.scratch_text = 0;
            }
            if (state.message == WM_PAINT)
            {
                if (auto* win = c.proc.windows.get(state.window))
                {
                    (void)handle_NtGdiFlush(c);
                    present_existing_guest_window_surface(c, *win);
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

            return converted_text_length.value_or(c.get_callback_result<uint64_t>());
        }

        bool read_combo_box_info(const syscall_context& c, const emulator_pointer combo_box_info, const size_t required_size,
                                 std::array<std::byte, 64>& data)
        {
            if (!c.win_emu.memory.try_read_memory(combo_box_info, data.data(), required_size))
            {
                set_guest_last_error(c, 998);
                return false;
            }

            uint32_t cb_size{};
            std::memcpy(&cb_size, data.data(), sizeof(cb_size));
            if (cb_size != required_size)
            {
                set_guest_last_error(c, 87);
                return false;
            }

            return true;
        }

        bool write_native_combo_box_info(const syscall_context& c, const window& combo_box, const emulator_pointer combo_box_info,
                                         const size_t required_size, std::array<std::byte, 64>& data)
        {
            const window* item = nullptr;
            const window* list = nullptr;
            for (const auto& window_entry : c.proc.windows)
            {
                const auto& child = window_entry.second;
                const auto class_name = std::u16string_view{child.class_name};
                if (!list && (child.parent_handle == combo_box.handle || child.owner_handle == combo_box.handle) &&
                    utils::string::equals_ignore_case(class_name, std::u16string_view{u"ComboLBox"}))
                {
                    list = &child;
                }
                else if (!item && child.parent_handle == combo_box.handle &&
                         (utils::string::equals_ignore_case(class_name, std::u16string_view{u"Edit"}) ||
                          utils::string::equals_ignore_case(class_name, std::u16string_view{u"Static"})))
                {
                    item = &child;
                }
            }

            const auto client = get_client_rect(combo_box);
            int32_t button_width{};
            c.proc.user_handles.get_server_info().access(
                [&](const USER_SERVERINFO& server_info) { button_width = std::max(0, server_info.systemMetrics[k_sm_cxvscroll_index]); });
            const bool simple = (combo_box.style & k_cbs_type_mask) == k_cbs_simple;
            const LONG button_left = simple ? client.right : std::max<LONG>(client.left, client.right - button_width);
            const RECT button_rect{
                .left = button_left,
                .top = client.top,
                .right = client.right,
                .bottom = client.bottom,
            };
            RECT item_rect = item ? RECT{.left = item->x, .top = item->y, .right = item->x + item->width, .bottom = item->y + item->height}
                                  : RECT{.left = client.left, .top = client.top, .right = button_left, .bottom = client.bottom};
            item_rect.right = std::max(item_rect.left, std::min(item_rect.right, button_left));
            uint32_t state_button = 0;
            if (simple)
            {
                state_button = k_state_system_invisible;
            }
            else if (list && (list->style & WS_VISIBLE) != 0)
            {
                state_button = k_state_system_pressed;
            }
            const uint64_t hwnd_item = item ? item->handle : 0;
            const uint64_t hwnd_list = list ? list->handle : 0;

            std::memcpy(data.data() + 4, &item_rect, sizeof(item_rect));
            std::memcpy(data.data() + 20, &button_rect, sizeof(button_rect));
            std::memcpy(data.data() + 36, &state_button, sizeof(state_button));

            if (c.proc.is_wow64_process)
            {
                const auto combo_handle = static_cast<uint32_t>(combo_box.handle);
                const auto item_handle = static_cast<uint32_t>(hwnd_item);
                const auto list_handle = static_cast<uint32_t>(hwnd_list);
                std::memcpy(data.data() + 40, &combo_handle, sizeof(combo_handle));
                std::memcpy(data.data() + 44, &item_handle, sizeof(item_handle));
                std::memcpy(data.data() + 48, &list_handle, sizeof(list_handle));
            }
            else
            {
                std::memcpy(data.data() + 40, &combo_box.handle, sizeof(combo_box.handle));
                std::memcpy(data.data() + 48, &hwnd_item, sizeof(hwnd_item));
                std::memcpy(data.data() + 56, &hwnd_list, sizeof(hwnd_list));
            }

            if (!c.win_emu.memory.try_write_memory(combo_box_info, data.data(), required_size))
            {
                set_guest_last_error(c, 998);
                return false;
            }

            return true;
        }

        BOOL handle_NtUserGetComboBoxInfo(const syscall_context& c, const hwnd combo_box, const emulator_pointer combo_box_info)
        {
            auto* win = c.proc.windows.get(combo_box);
            if (!win)
            {
                set_guest_last_error(c, 1400);
                return FALSE;
            }
            if (combo_box_info == 0)
            {
                set_guest_last_error(c, 998);
                return FALSE;
            }

            const size_t required_size = c.proc.is_wow64_process ? 52 : 64;
            std::array<std::byte, 64> data{};
            if (!read_combo_box_info(c, combo_box_info, required_size, data))
            {
                return FALSE;
            }

            const auto normalized_class = normalize_builtin_window_class_name(win->class_name);
            if (utils::string::equals_ignore_case(normalized_class, std::u16string_view{u"ComboBox"}))
            {
                return write_native_combo_box_info(c, *win, combo_box_info, required_size, data) ? TRUE : FALSE;
            }

            if (!c.win_emu.memory.try_write_memory(combo_box_info, data.data(), required_size))
            {
                set_guest_last_error(c, 998);
                return FALSE;
            }

            message_call_state state{};
            state.window = combo_box;
            state.message = k_cb_getcomboboxinfo;
            dispatch_window_message(c, callback_id::NtUserGetComboBoxInfo, std::move(state), *win, k_cb_getcomboboxinfo, 0, combo_box_info);
            return FALSE;
        }

        BOOL completion_NtUserGetComboBoxInfo(const syscall_context& c, const hwnd /*combo_box*/, const emulator_pointer /*combo_box_info*/)
        {
            return c.get_callback_result<uint64_t>() != 0 ? TRUE : FALSE;
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

        DWORD handle_NtUserGetMessagePos(const syscall_context& c)
        {
            return c.vcpu.active_thread ? c.vcpu.active_thread->current_message_position : 0;
        }

        BOOL handle_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                     const UINT msg_filter_min, const UINT msg_filter_max)
        {
            if (dispatch_pending_sent_window_message(c, callback_id::NtUserGetMessage))
            {
                return {};
            }

            if (dispatch_low_level_hook(c, callback_id::NtUserGetMessage))
            {
                return {};
            }

            auto& t = c.thread();

            if (auto pending_msg = t.peek_pending_message(c.win_emu, hwnd, msg_filter_min, msg_filter_max, message_retrieval_mode::get))
            {
                message.write(*pending_msg);
                t.record_current_message(*pending_msg);
                set_thread_window_context(c, pending_msg->window);
                if (dispatch_get_message_hook(c, callback_id::NtUserGetMessage, *pending_msg, PM_REMOVE))
                {
                    return {};
                }
                return pending_msg->message != WM_QUIT ? TRUE : FALSE;
            }

            t.await_msg = {message, hwnd, msg_filter_min, msg_filter_max};
            t.remember_message_wait(c.emu, c.win_emu.abi());

            c.win_emu.yield_thread(c.vcpu, false);
            return {};
        }

        BOOL completion_NtUserGetMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                         const UINT msg_filter_min, const UINT msg_filter_max)
        {
            if (c.current_completion_state &&
                c.get_completion_state<message_retrieval_state>().callback == message_retrieval_callback::get_message_hook)
            {
                const auto hooked = complete_get_message_hook(c);
                message.write(hooked);
                return hooked.message != WM_QUIT ? TRUE : FALSE;
            }

            if (c.current_completion_state)
            {
                complete_active_sent_window_message(c);
            }
            else
            {
                complete_low_level_hook(c);
            }
            return handle_NtUserGetMessage(c, message, hwnd, msg_filter_min, msg_filter_max);
        }

        BOOL handle_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                      const UINT msg_filter_min, const UINT msg_filter_max, const UINT remove_message)
        {
            if (dispatch_pending_sent_window_message(c, callback_id::NtUserPeekMessage))
            {
                return {};
            }

            if (dispatch_low_level_hook(c, callback_id::NtUserPeekMessage))
            {
                return {};
            }

            auto& t = c.thread();

            const bool should_remove = (remove_message & PM_REMOVE) != 0;
            const auto mode = should_remove ? message_retrieval_mode::remove : message_retrieval_mode::peek;
            std::optional<msg> pending_msg = t.peek_pending_message(c.win_emu, hwnd, msg_filter_min, msg_filter_max, mode);

            if (pending_msg)
            {
                message.write(*pending_msg);
                t.record_current_message(*pending_msg);
                set_thread_window_context(c, pending_msg->window);
                if (dispatch_get_message_hook(c, callback_id::NtUserPeekMessage, *pending_msg, should_remove ? PM_REMOVE : PM_NOREMOVE))
                {
                    return {};
                }
                return TRUE;
            }

            return FALSE;
        }

        BOOL completion_NtUserPeekMessage(const syscall_context& c, const emulator_object<msg> message, const hwnd hwnd,
                                          const UINT msg_filter_min, const UINT msg_filter_max, const UINT remove_message)
        {
            if (c.current_completion_state &&
                c.get_completion_state<message_retrieval_state>().callback == message_retrieval_callback::get_message_hook)
            {
                message.write(complete_get_message_hook(c));
                return TRUE;
            }

            if (c.current_completion_state)
            {
                complete_active_sent_window_message(c);
            }
            else
            {
                complete_low_level_hook(c);
            }
            return handle_NtUserPeekMessage(c, message, hwnd, msg_filter_min, msg_filter_max, remove_message);
        }

        BOOL handle_NtUserWaitMessage(const syscall_context& c)
        {
            if (dispatch_low_level_hook(c, callback_id::NtUserWaitMessage))
            {
                return {};
            }

            auto& t = c.thread();
            if (!t.pending_sent_messages.empty() || t.peek_pending_message(c.win_emu))
            {
                return TRUE;
            }

            t.await_msg_mask = QS_ALLINPUT;
            t.await_msg_ready_status = TRUE;
            t.remember_message_wait(c.emu, c.win_emu.abi());
            c.win_emu.yield_thread(c.vcpu, false);
            return {};
        }

        BOOL completion_NtUserWaitMessage(const syscall_context& c)
        {
            complete_low_level_hook(c);
            return handle_NtUserWaitMessage(c);
        }

        BOOL handle_NtUserReplyMessage(const syscall_context& c, const uint64_t result)
        {
            auto& messages = c.thread().active_sent_messages;
            if (messages.empty())
            {
                return FALSE;
            }

            // Windows replies to the innermost active cross-thread send even when a nested same-thread
            // window procedure calls ReplyMessage.
            auto& message = messages.back();
            if (message.replied)
            {
                return FALSE;
            }

            copy_message_text_result(c, message.message, message.w_param, message.l_param, message.ansi, message.scratch_text);
            complete_sent_window_message(c, message, result);
            return TRUE;
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

        BOOL handle_NtUserLockWindowUpdate(const syscall_context& c, const hwnd window)
        {
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_invalid_window_handle = 1400;
            constexpr uint32_t error_screen_already_locked = 1440;

            const auto* target = window != 0 ? c.proc.windows.get(window) : nullptr;
            // win32k validates the low 32 bits of a window handle only.
            if (window != 0 && (!target || static_cast<uint32_t>(target->handle) != static_cast<uint32_t>(window)))
            {
                set_guest_last_error(c, error_invalid_window_handle);
                return FALSE;
            }

            if (c.proc.update_locked_window == 0)
            {
                if (!target)
                {
                    set_guest_last_error(c, error_invalid_parameter);
                    return FALSE;
                }

                c.proc.update_locked_window = target->handle;
                c.proc.update_lock_thread_id = c.vcpu.active_thread->id;
                return TRUE;
            }

            if (c.proc.update_lock_thread_id != c.vcpu.active_thread->id)
            {
                set_guest_last_error(c, error_screen_already_locked);
                return FALSE;
            }

            if (target)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return FALSE;
            }

            c.proc.release_update_lock(c.proc.update_locked_window);
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

        BOOL handle_NtUserGetUpdateRect(const syscall_context& c, const hwnd hwnd, const emulator_object<RECT> rect, const BOOL /*erase*/)
        {
            const auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            if (rect)
            {
                rect.write(win->update_pending ? win->update_rect : RECT{});
            }

            return win->update_pending ? TRUE : FALSE;
        }

        int32_t handle_NtUserGetUpdateRgn(const syscall_context& c, const hwnd hwnd, const handle region, const BOOL)
        {
            const auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return 0;
            }

            const auto rect = win->update_pending ? win->update_rect : RECT{};
            if (!set_gdi_region_rect(c, region, rect))
            {
                return 0;
            }

            return rect.left < rect.right && rect.top < rect.bottom ? 2 : 1;
        }

        void collect_pending_paint_tree(const syscall_context& c, window& win, std::vector<uint64_t>& order)
        {
            if (!c.proc.is_window_effectively_visible(win.handle))
            {
                return;
            }

            if (win.update_pending && win.thread_id == c.vcpu.active_thread->id)
            {
                order.push_back(static_cast<uint64_t>(win.handle));
            }

            for (auto& [index, child] : c.proc.windows)
            {
                (void)index;
                if (child.parent_handle == win.handle)
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

            if (win->thread_id != c.vcpu.active_thread->id)
            {
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
                    utils::string::copy(dev.DeviceString, emulated_display::description);
                    utils::string::copy(dev.DeviceID, emulated_display::hardware_id);
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

        constexpr uint32_t k_display_bits_per_pixel = 32;
        constexpr uint32_t k_display_frequency = 60;

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
                    dm.dmBitsPerPel = k_display_bits_per_pixel;
                    dm.dmDisplayFrequency = k_display_frequency;
                });
            };

            if (mode_num == ENUM_CURRENT_SETTINGS)
            {
                fill_mode(c.proc.display_width, c.proc.display_height);
                return STATUS_SUCCESS;
            }

            if (mode_num == ENUM_REGISTRY_SETTINGS)
            {
                fill_mode(c.proc.registry_display_width, c.proc.registry_display_height);
                return STATUS_SUCCESS;
            }

            auto offered = display_modes | std::views::filter([&](const display_mode& mode) {
                               return mode.fits_within(c.win_emu.max_display_mode()); //
                           });
            DWORD index = 0;
            for (const auto& mode : offered)
            {
                if (index++ == mode_num)
                {
                    fill_mode(mode.width, mode.height);
                    return STATUS_SUCCESS;
                }
            }

            return STATUS_UNSUCCESSFUL;
        }

        // Fullscreen games switch the display to their resolution and take the screen size from then on, e.g. to convert
        // screen coordinates of the cursor. A null mode returns to the registry mode.
        LONG handle_NtUserChangeDisplaySettings(const syscall_context& c,
                                                const emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> device_name,
                                                const emulator_object<EMU_DEVMODEW> dev_mode, const hwnd /*window*/, const DWORD flags,
                                                const uint64_t /*param*/)
        {
            constexpr LONG disp_change_successful = 0;
            constexpr LONG disp_change_badmode = -2;
            constexpr LONG disp_change_badparam = -5;
            constexpr DWORD cds_updateregistry = 0x1;
            constexpr DWORD cds_test = 0x2;
            constexpr DWORD cds_noreset = 0x10000000;
            constexpr DWORD dm_bitsperpel = 0x40000;
            constexpr DWORD dm_pelswidth = 0x80000;
            constexpr DWORD dm_pelsheight = 0x100000;
            constexpr DWORD dm_displayfrequency = 0x400000;

            if (const auto name = device_name ? read_unicode_string(c.emu, device_name) : u""; !name.empty() && name != u"\\\\.\\DISPLAY1")
            {
                return disp_change_badparam;
            }

            auto width = c.proc.registry_display_width;
            auto height = c.proc.registry_display_height;
            if (dev_mode)
            {
                const auto mode = dev_mode.read();
                width = (mode.dmFields & dm_pelswidth) != 0 ? mode.dmPelsWidth : c.proc.display_width;
                height = (mode.dmFields & dm_pelsheight) != 0 ? mode.dmPelsHeight : c.proc.display_height;

                // A frequency of 0 or 1 asks for the display's default.
                if (((mode.dmFields & dm_bitsperpel) != 0 && mode.dmBitsPerPel != k_display_bits_per_pixel) ||
                    ((mode.dmFields & dm_displayfrequency) != 0 && mode.dmDisplayFrequency > 1 &&
                     mode.dmDisplayFrequency != k_display_frequency))
                {
                    return disp_change_badmode;
                }
            }

            const display_mode requested{.width = width, .height = height};
            if (!requested.fits_within(c.win_emu.max_display_mode()) || std::ranges::find(display_modes, requested) == display_modes.end())
            {
                return disp_change_badmode;
            }

            if ((flags & cds_test) != 0)
            {
                return disp_change_successful;
            }

            if ((flags & cds_updateregistry) != 0)
            {
                c.proc.registry_display_width = width;
                c.proc.registry_display_height = height;
            }

            if ((flags & cds_noreset) == 0)
            {
                c.proc.set_display_mode(c.win_emu, width, height);
                c.win_emu.apply_cursor_capture();
            }

            return disp_change_successful;
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

        NTSTATUS handle_NtGdiGetCurrentDpiInfo(const syscall_context& c, const handle monitor,
                                               const emulator_object<EMU_DPI_INFORMATION> info)
        {
            if (monitor != c.proc.default_monitor_handle || !info)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto display_info = c.proc.user_handles.get_display_info().read();
            const emulator_object<USER_MONITOR> monitor_obj(c.emu, display_info.pPrimaryMonitor);
            if (!monitor_obj)
            {
                return STATUS_INVALID_PARAMETER;
            }

            const auto rect = monitor_obj.read().rcMonitor;
            const auto width = static_cast<UINT32>(rect.right - rect.left);
            const auto height = static_cast<UINT32>(rect.bottom - rect.top);

            EMU_DPI_INFORMATION dpi{};
            dpi.ScaleFactor = 100;
            dpi.ScaleFactorY = 100;
            dpi.MonitorWidth = width;
            dpi.MonitorHeight = height;
            dpi.PhysicalHeight = height;
            dpi.PhysicalWidth = width;
            dpi.DpiX = 96;
            dpi.DpiY = 96;
            info.write(dpi);
            return STATUS_SUCCESS;
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
                auto* desktop = c.proc.get_object_as<sogen::desktop>(resolve_desktop(c, handle));
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

            const auto index = static_cast<uint32_t>(handle.value.id);

            if (index == 0 || index >= user_handle_table::MAX_HANDLE_INDICES)
            {
                return 0;
            }

            const auto handle_entry = c.proc.user_handles.get_handle_table().read(user_handle_table::handle_index_to_ahe_slot(index));
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

            rect.write(get_screen_window_rect(c, *win));
            return TRUE;
        }

        // A window that stops being a surface root draws into its new root's surface from now on, so its own surface is
        // dropped; either way it repaints where it now appears.
        void update_window_relationship(const syscall_context& c, window& win, const bool was_surface_root)
        {
            const bool surface_root = c.proc.is_surface_root(win);
            c.proc.sync_guest_window_rects(win);
            if (was_surface_root && !surface_root)
            {
                c.proc.gdi_window_surfaces.erase(static_cast<uint32_t>(win.handle));
            }

            if (win.host_surface_window)
            {
                c.win_emu.ui().set_window_relationship(win.handle, surface_root ? 0 : win.parent_handle,
                                                       surface_root ? win.owner_handle : 0, surface_root);
            }

            if ((win.style & WS_VISIBLE) != 0)
            {
                invalidate_window(c, win, std::nullopt, true, child_redraw_scope::all);
            }
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

            const auto* previous_parent = c.proc.windows.get(old_parent);
            if (effective_parent->handle == desktop && previous_parent &&
                utils::string::equals_ignore_case(std::u16string_view{child->class_name}, std::u16string_view{u"ComboLBox"}) &&
                normalize_builtin_window_class_name(previous_parent->class_name) == u"ComboBox")
            {
                child->owner_handle = previous_parent->handle;
            }

            const bool was_surface_root = c.proc.is_surface_root(*child);
            const RECT vacated_area{
                .left = child->x, .top = child->y, .right = child->x + child->width, .bottom = child->y + child->height};
            child->parent_handle = effective_parent->handle;
            child->guest.access([&](USER_WINDOW& guest_win) {
                guest_win.spwndParent = effective_parent->guest.value();
                guest_win.spwndPrev = 0;
                guest_win.spwndNext = 0;
            });

            append_to_parent();
            update_window_relationship(c, *child, was_surface_root);

            // Windows hides the window before it changes the parent, so the old parent repaints what the window covered.
            if (auto* vacated_parent = c.proc.windows.get(old_parent);
                vacated_parent && vacated_parent != effective_parent && (child->style & WS_VISIBLE) != 0)
            {
                invalidate_window(c, *vacated_parent, vacated_area, true);
            }

            return old_parent;
        }

        BOOL handle_NtUserSetWindowPos(const syscall_context& c, const hwnd hWnd, const hwnd hwnd_insert_after, const int x, const int y,
                                       const int cx, const int cy, const UINT flags)
        {
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                return FALSE;
            }

            EMU_WINDOWPOS position{
                .hwnd = hWnd,
                .hwndInsertAfter = hwnd_insert_after,
                .x = x,
                .y = y,
                .cx = cx,
                .cy = cy,
                .flags = flags,
            };

            window_position_state state{};
            state.window_pos_alloc = c.emu.push_stack(position);

            if ((flags & SWP_NOSENDCHANGING) == 0)
            {
                const auto window_pos_address = state.window_pos_alloc.address();
                dispatch_window_message(c, callback_id::NtUserSetWindowPos, std::move(state), *win, WM_WINDOWPOSCHANGING, 0,
                                        window_pos_address);
            }
            else
            {
                apply_set_window_position(c, *win, state, position);

                if (state.message_queue.empty())
                {
                    release_window_position_allocations(c, state);
                    return TRUE;
                }

                dispatch_next_message(c, callback_id::NtUserSetWindowPos, std::move(state), *win, state.message_queue);
            }

            return {};
        }

        BOOL completion_NtUserSetWindowPos(const syscall_context& c, const hwnd hWnd, const hwnd /*hwnd_insert_after*/, const int /*x*/,
                                           const int /*y*/, const int /*cx*/, const int /*cy*/, const UINT /*flags*/)
        {
            auto& state = c.get_completion_state<window_position_state>();
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                release_window_position_allocations(c, state);
                return FALSE;
            }

            if (!state.position_applied)
            {
                const auto position = resolve_window_position_callback(c, state.window_pos_alloc.address());
                apply_set_window_position(c, *win, state, position);
            }

            if (!state.message_queue.empty())
            {
                dispatch_next_message(c, callback_id::NtUserSetWindowPos, std::move(state), *win, state.message_queue);
                return {};
            }

            release_window_position_allocations(c, state);
            return TRUE;
        }

        uint64_t handle_NtUserBeginDeferWindowPos(const syscall_context& c, const int count)
        {
            if (count < 0)
            {
                set_guest_last_error(c, 87);
                return 0;
            }

            deferred_window_positions batch{};
            batch.positions.reserve(static_cast<size_t>(count));
            return c.proc.deferred_window_position_batches.store(std::move(batch)).bits;
        }

        uint64_t handle_NtUserDeferWindowPosAndBand(const syscall_context& c, const uint64_t batch_handle, const hwnd window,
                                                    const hwnd window_insert_after, const int x, const int y, const int width,
                                                    const int height, const UINT flags, const uint32_t /*band*/, const BOOL /*use_band*/)
        {
            auto* batch = c.proc.deferred_window_position_batches.get(batch_handle);
            if (!batch)
            {
                set_guest_last_error(c, 1405);
                return 0;
            }

            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                set_guest_last_error(c, 1400);
                return 0;
            }

            if (!batch->positions.empty() && batch->parent_handle != win->parent_handle)
            {
                set_guest_last_error(c, 1441);
                return 0;
            }

            batch->parent_handle = win->parent_handle;

            batch->positions.push_back({
                .hwnd = window,
                .hwndInsertAfter = window_insert_after,
                .x = x,
                .y = y,
                .cx = width,
                .cy = height,
                .flags = flags,
            });
            return batch_handle;
        }

        BOOL advance_deferred_window_positions(const syscall_context& c, deferred_window_position_state& state)
        {
            while (state.next_position < state.positions.size())
            {
                const auto& position = state.positions[state.next_position++];
                auto* win = c.proc.windows.get(position.hwnd);
                if (!win)
                {
                    continue;
                }

                state.current_window = position.hwnd;
                state.window_pos_alloc = c.emu.push_stack(position);
                state.changed_window_pos_alloc = {};
                state.message_queue.clear();
                state.position_applied = false;

                if ((position.flags & SWP_NOSENDCHANGING) == 0)
                {
                    dispatch_window_message(c, callback_id::NtUserEndDeferWindowPosEx, std::move(state), *win, WM_WINDOWPOSCHANGING, 0,
                                            state.window_pos_alloc.address());
                    return {};
                }

                apply_set_window_position(c, *win, state, position);
                if (!state.message_queue.empty())
                {
                    dispatch_next_message(c, callback_id::NtUserEndDeferWindowPosEx, std::move(state), *win, state.message_queue);
                    return {};
                }

                release_window_position_allocations(c, state);
            }

            return TRUE;
        }

        BOOL handle_NtUserEndDeferWindowPosEx(const syscall_context& c, const uint64_t batch_handle, const BOOL /*async*/)
        {
            auto* batch = c.proc.deferred_window_position_batches.get(batch_handle);
            if (!batch)
            {
                set_guest_last_error(c, 1405);
                return FALSE;
            }

            deferred_window_position_state state{};
            state.positions = std::move(batch->positions);
            c.proc.deferred_window_position_batches.erase(batch_handle);
            return advance_deferred_window_positions(c, state);
        }

        BOOL completion_NtUserEndDeferWindowPosEx(const syscall_context& c, const uint64_t /*batch_handle*/, const BOOL /*async*/)
        {
            auto& state = c.get_completion_state<deferred_window_position_state>();
            auto* win = c.proc.windows.get(state.current_window);
            if (!win)
            {
                release_window_position_allocations(c, state);
                return advance_deferred_window_positions(c, state);
            }

            if (!state.position_applied)
            {
                const auto position = resolve_window_position_callback(c, state.window_pos_alloc.address());
                apply_set_window_position(c, *win, state, position);
            }

            if (!state.message_queue.empty())
            {
                dispatch_next_message(c, callback_id::NtUserEndDeferWindowPosEx, std::move(state), *win, state.message_queue);
                return {};
            }

            release_window_position_allocations(c, state);
            return advance_deferred_window_positions(c, state);
        }

        BOOL handle_NtUserSetForegroundWindow(const syscall_context& c, const hwnd hwnd)
        {
            const auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return FALSE;
            }

            if ((win->style & WS_CHILD) != 0 && (win->style & WS_POPUP) == 0)
            {
                return FALSE;
            }

            c.win_emu.set_foreground_window(hwnd);
            return TRUE;
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

        // The focus moves before either window hears of it: WM_KILLFOCUS goes to the old window with the new one in wParam,
        // then WM_SETFOCUS to the new window with the old one. Either window procedure can move the focus again or destroy a
        // window, so each message is only sent while its window exists and the focus change it reports is still current.
        hwnd continue_focus_change(const syscall_context& c, focus_change_state state)
        {
            const auto& thread = c.thread();
            const auto notifies = [&](const uint64_t window) -> const sogen::window* {
                const auto* win = c.proc.windows.get(window);
                return win && win->thread_id == thread.id && thread.focus_window == state.next ? win : nullptr;
            };

            if (!state.kill_focus_sent)
            {
                state.kill_focus_sent = true;
                if (const auto* old_focus = notifies(state.previous))
                {
                    const auto new_focus = state.next;
                    dispatch_window_message(c, callback_id::NtUserSetFocus, std::move(state), *old_focus, WM_KILLFOCUS, new_focus);
                    return {};
                }
            }

            if (!state.set_focus_sent)
            {
                state.set_focus_sent = true;
                if (const auto* new_focus = notifies(state.next))
                {
                    const auto old_focus = state.previous;
                    dispatch_window_message(c, callback_id::NtUserSetFocus, std::move(state), *new_focus, WM_SETFOCUS, old_focus);
                    return {};
                }
            }

            return state.previous;
        }

        // A window cannot take the focus while it or any window up to its top-level window is disabled or minimized.
        bool can_take_focus(const syscall_context& c, const window& target)
        {
            const auto* current = &target;
            for (size_t depth = 0; current && depth < c.proc.windows.size(); ++depth)
            {
                if ((current->style & (WS_DISABLED | WS_MINIMIZE)) != 0)
                {
                    return false;
                }
                if (c.proc.is_surface_root(*current))
                {
                    return true;
                }
                current = c.proc.windows.get(current->parent_handle);
            }
            return true;
        }

        hwnd handle_NtUserSetFocus(const syscall_context& c, const hwnd hwnd)
        {
            auto& thread = c.thread();
            if (hwnd != 0)
            {
                const auto* target = c.proc.windows.get(hwnd);
                if (!target)
                {
                    set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                    return 0;
                }
                if (target->thread_id != thread.id)
                {
                    set_guest_last_error(c, 5); // ERROR_ACCESS_DENIED
                    return 0;
                }
                if (!can_take_focus(c, *target))
                {
                    set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                    return 0;
                }
            }

            const auto previous = thread.focus_window;
            if (previous == hwnd)
            {
                return previous;
            }

            thread.set_focus_window(hwnd);
            if (hwnd != 0)
            {
                set_thread_window_context(c, hwnd);
            }

            focus_change_state state{};
            state.previous = previous;
            state.next = hwnd;
            return continue_focus_change(c, std::move(state));
        }

        hwnd completion_NtUserSetFocus(const syscall_context& c, const hwnd /*hwnd*/)
        {
            return continue_focus_change(c, std::move(c.get_completion_state<focus_change_state>()));
        }

        BOOL handle_NtUserZapActiveAndFocus(const syscall_context& c)
        {
            auto& thread = c.thread();
            thread.set_active_window(0);
            thread.set_focus_window(0);
            return TRUE;
        }

        emulator_pointer handle_NtUserSetClassLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                      BOOL /*Ansi*/)
        {
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                return 0;
            }

            const auto entry = c.proc.classes.find(win->class_key);
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
                // A positive index addresses the class's extra bytes. win32kfull's xxxSetClassLongPtr fails one whose 8 bytes
                // do not fit in cbClsExtra with ERROR_INVALID_INDEX, and otherwise swaps the value in place.
                if (nIndex >= 0)
                {
                    const auto offset = static_cast<uint32_t>(nIndex);
                    if (offset + 8 < offset || offset + 8 > static_cast<uint32_t>(cls.cbClsExtra))
                    {
                        set_guest_last_error(c, 1413); // ERROR_INVALID_INDEX
                        return 0;
                    }

                    const auto address = entry->second.guest_obj_addr + process_context::user_class_extra_offset + offset;
                    old_value = c.win_emu.memory.read_memory<uint64_t>(address);
                    c.win_emu.memory.write_memory(address, uint64_t{dwNewLong});
                    return old_value;
                }

                // GCL_CBCLSEXTRA / GCL_CBWNDEXTRA / GCLP_MENUNAME: not modeled.
                break;
            }

            publish_class_to_guest(c.win_emu.memory, entry->second);
            return old_value;
        }

        emulator_pointer begin_window_style_change(const syscall_context& c, const handle hWnd, const int nIndex, const uint32_t new_style,
                                                   const callback_id callback)
        {
            const auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                return 0;
            }

            window_style_change_state state{};
            state.old_style = nIndex == GWL_STYLE ? win->style : win->ex_style;
            state.style_alloc = c.emu.push_stack(STYLESTRUCT{.styleOld = state.old_style, .styleNew = new_style});
            const auto address = state.style_alloc.address();
            dispatch_window_message(c, callback, std::move(state), *win, WM_STYLECHANGING,
                                    static_cast<uint64_t>(static_cast<int64_t>(nIndex)), address);
            return 0;
        }

        emulator_pointer handle_NtUserSetWindowLongPtr(const syscall_context& c, handle hWnd, int nIndex, emulator_pointer dwNewLong,
                                                       BOOL /*Ansi*/)
        {
            if (nIndex == GWL_STYLE || nIndex == GWL_EXSTYLE)
            {
                return begin_window_style_change(c, hWnd, nIndex, static_cast<uint32_t>(dwNewLong), callback_id::NtUserSetWindowLongPtr);
            }

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

        emulator_pointer complete_window_style_change(const syscall_context& c, const handle hWnd, const int nIndex,
                                                      const callback_id callback)
        {
            auto& state = c.get_completion_state<window_style_change_state>();
            const auto old_style = state.old_style;
            auto* win = c.proc.windows.get(hWnd);
            if (!win)
            {
                c.emu.pop_stack(state.style_alloc);
                return 0;
            }

            if (!state.style_applied)
            {
                auto styles = c.emu.read_memory<STYLESTRUCT>(state.style_alloc.address());
                styles.styleNew = filter_supported_window_style_change(*win, nIndex, styles.styleNew);
                c.emu.write_memory(state.style_alloc.address(), styles);
                if (nIndex == GWL_STYLE)
                {
                    win->style = styles.styleNew;
                    win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwStyle = styles.styleNew; });
                }
                else
                {
                    win->ex_style = styles.styleNew;
                    win->guest.access([&](USER_WINDOW& guest_win) { guest_win.dwExStyle = styles.styleNew; });
                }
                c.proc.sync_guest_window_rects(*win);

                state.style_applied = true;
                const auto address = state.style_alloc.address();
                dispatch_window_message(c, callback, std::move(state), *win, WM_STYLECHANGED,
                                        static_cast<uint64_t>(static_cast<int64_t>(nIndex)), address);
                return 0;
            }

            if (nIndex == GWL_STYLE)
            {
                const bool was_enabled = (old_style & WS_DISABLED) == 0;
                const bool is_enabled = (win->style & WS_DISABLED) == 0;
                if (was_enabled != is_enabled)
                {
                    if (win->host_surface_window)
                    {
                        c.win_emu.ui().set_window_enabled(win->handle, is_enabled);
                    }
                }

                const bool was_visible = (old_style & WS_VISIBLE) != 0;
                const bool is_visible = (win->style & WS_VISIBLE) != 0;
                if (was_visible != is_visible)
                {
                    if (win->host_surface_window)
                    {
                        c.win_emu.ui().set_window_visible(win->handle, is_visible, false);
                    }
                }
            }

            c.emu.pop_stack(state.style_alloc);
            return old_style;
        }

        emulator_pointer completion_NtUserSetWindowLongPtr(const syscall_context& c, const handle hWnd, const int nIndex,
                                                           const emulator_pointer /*dwNewLong*/, const BOOL /*Ansi*/)
        {
            return complete_window_style_change(c, hWnd, nIndex, callback_id::NtUserSetWindowLongPtr);
        }

        uint32_t handle_NtUserSetWindowLong(const syscall_context& c, handle hWnd, int nIndex, uint32_t dwNewLong, BOOL Ansi)
        {
            if (nIndex == GWL_STYLE || nIndex == GWL_EXSTYLE)
            {
                return static_cast<uint32_t>(begin_window_style_change(c, hWnd, nIndex, dwNewLong, callback_id::NtUserSetWindowLong));
            }

            const auto oldValue = handle_NtUserSetWindowLongPtr(c, hWnd, nIndex, static_cast<emulator_pointer>(dwNewLong), Ansi);
            return static_cast<uint32_t>(oldValue);
        }

        uint32_t completion_NtUserSetWindowLong(const syscall_context& c, const handle hWnd, const int nIndex, const uint32_t /*dwNewLong*/,
                                                const BOOL /*Ansi*/)
        {
            return static_cast<uint32_t>(complete_window_style_change(c, hWnd, nIndex, callback_id::NtUserSetWindowLong));
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

        BOOL handle_NtUserIsTopLevelWindow(const syscall_context& c, const hwnd window)
        {
            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                return FALSE;
            }

            // Owned popups still parent to the desktop; do not consult owner_handle.
            return win->parent_handle == c.proc.default_desktop_window_handle.bits ? TRUE : FALSE;
        }

        BOOL handle_NtUserRedrawWindow(const syscall_context& c, const hwnd hwnd, const emulator_object<RECT> update_rect,
                                       const uint64_t /*update_rgn*/, const UINT flags)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                return FALSE;
            }

            auto scope = child_redraw_scope::unclipped;
            if ((flags & RDW_NOCHILDREN) != 0)
            {
                scope = child_redraw_scope::none;
            }
            else if ((flags & RDW_ALLCHILDREN) != 0)
            {
                scope = child_redraw_scope::all;
            }

            std::optional<RECT> rect{};
            if (update_rect)
            {
                rect = update_rect.read();
            }

            if ((flags & RDW_VALIDATE) != 0)
            {
                validate_window(*win);
                validate_children(c, *win, rect.value_or(get_client_rect(*win)), scope);
            }

            if ((flags & RDW_INVALIDATE) != 0)
            {
                const bool erase = (flags & RDW_ERASE) != 0 && (flags & RDW_NOERASE) == 0;
                invalidate_window(c, *win, rect, erase, scope);
            }

            if ((flags & RDW_INTERNALPAINT) != 0)
            {
                set_internal_paint_pending(c, *win, true);
            }

            if ((flags & RDW_NOINTERNALPAINT) != 0)
            {
                set_internal_paint_pending(c, *win, false);
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

        BOOL handle_NtUserSetSystemMenu(const syscall_context& c, const hwnd hwnd, const hmenu menu)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                set_guest_last_error(c, 1400);
                return FALSE;
            }

            if (menu == 0 || !c.proc.menus.get(menu))
            {
                set_guest_last_error(c, 1401);
                return FALSE;
            }

            if ((win->style & WS_SYSMENU) == 0)
            {
                return FALSE;
            }

            if (win->system_menu_handle != 0 && win->system_menu_handle != menu)
            {
                std::unordered_set<hmenu> visited{menu};
                destroy_menu_tree(c, win->system_menu_handle, visited);
            }

            win->system_menu_handle = menu;
            return TRUE;
        }

        BOOL handle_NtUserSetSysMenu(const syscall_context& c, const hwnd hwnd)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                set_guest_last_error(c, 1400);
                return FALSE;
            }

            // TODO: enable or disable system menu items (minimize, restore, size, move, ...) based on the window's state

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

            if (revert != FALSE)
            {
                if (win->system_menu_handle != 0)
                {
                    std::unordered_set<hmenu> visited;
                    destroy_menu_tree(c, win->system_menu_handle, visited);
                }

                win->system_menu_handle = 0;
                (void)ensure_system_menu(c, *win);
                return 0;
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
            if (!name)
            {
                set_guest_last_error(c, 6); // ERROR_INVALID_HANDLE
                return 0;
            }

            if (!atom_name)
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

        // user32 derives the mode count from the path count itself; the syscall only reports paths.
        NTSTATUS handle_NtUserGetDisplayConfigBufferSizes(const syscall_context& c, const UINT32 /*flags*/,
                                                          const emulator_object<UINT32> num_path_array_elements)
        {
            if (!num_path_array_elements)
            {
                return STATUS_INVALID_PARAMETER;
            }

            // Use non-throwing writes: a failed guest write (e.g. a caller passing a bogus output pointer)
            // must surface as an error to the caller, not abort the whole emulator with an unhandled
            // host-side memory exception.
            const UINT32 path_count = 1;
            if (!c.win_emu.memory.try_write_memory(num_path_array_elements.value(), &path_count, sizeof(path_count)))
            {
                return STATUS_INVALID_PARAMETER;
            }

            return STATUS_SUCCESS;
        }

        NTSTATUS handle_NtUserQueryDisplayConfig(const syscall_context& c, const UINT32 flags,
                                                 const emulator_object<UINT32> num_path_array_elements, const emulator_pointer path_array,
                                                 const emulator_object<UINT32> current_topology_id, const emulator_pointer /*reserved*/)
        {
            constexpr UINT32 qdc_database_current = 0x4;

            // One of QDC_ALL_PATHS, QDC_ONLY_ACTIVE_PATHS or QDC_DATABASE_CURRENT, plus QDC_VIRTUAL_MODE_AWARE and QDC_INCLUDE_HMD,
            // and on Windows 11 QDC_VIRTUAL_REFRESH_RATE_AWARE, which its SDK adds: win32kbase rejects any other bit (19041
            // outside 0x37, 26100 outside 0x77).
            const auto valid_flags = c.win_emu.version.is_build_after_or_equal(22000) ? 0x77U : 0x37U;
            const auto query = flags & 0x7;
            if ((flags & ~valid_flags) != 0 || (query != 0x1 && query != 0x2 && query != qdc_database_current))
            {
                return STATUS_INVALID_PARAMETER;
            }

            // On 19041-19045 DISPLAYCONFIG_PATH_INFO_INTERNAL is 200 bytes: user32 allocates and win32kbase probes and
            // copies 200 bytes per path. The fields written below keep their offsets there; only the struct is shorter.
            const bool windows_10_2004 = c.win_emu.version.is_build_within(19041, 19046);
            const uint64_t path_size = windows_10_2004 ? 200 : 216;

            // win32kbase probes every caller buffer before it writes any of them: the element count, then the whole
            // declared path array (ProbeForWrite of count * path size, ULONG-aligned outside WoW64), then the topology id.
            // Its exception handler sets the last error from the fault and fails the call with STATUS_INVALID_PARAMETER
            // (19041 and 26100).
            const auto probe_failed = [&] {
                set_guest_last_error(c, 998); // ERROR_NOACCESS, for a misaligned buffer as for an inaccessible one
                return STATUS_INVALID_PARAMETER;
            };

            if (!is_writable_range(c.win_emu, num_path_array_elements.value(), sizeof(UINT32)))
            {
                return probe_failed();
            }

            const auto num_paths = num_path_array_elements.read();
            if (num_paths == 0)
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (path_array % (c.proc.is_wow64_process ? 1 : sizeof(ULONG)) != 0 ||
                !is_writable_range(c.win_emu, path_array, path_size * num_paths))
            {
                return probe_failed();
            }

            // Only the current database query reports a topology, and the other queries must not ask for one.
            const bool database_current = query == qdc_database_current;
            if (database_current)
            {
                if (current_topology_id.value() % sizeof(UINT32) != 0 ||
                    !is_writable_range(c.win_emu, current_topology_id.value(), sizeof(UINT32)))
                {
                    return probe_failed();
                }
            }
            else if (current_topology_id)
            {
                return STATUS_INVALID_PARAMETER;
            }

            num_path_array_elements.write(1);

            if (database_current)
            {
                current_topology_id.write(0x1); // DISPLAYCONFIG_TOPOLOGY_INTERNAL
            }

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
            internal_path.targetSignalInfo.activeSize = {.cx = c.proc.display_width, .cy = c.proc.display_height};
            internal_path.targetSignalInfo.totalSize = {.cx = 2200, .cy = 1125};
            internal_path.targetSignalInfo.scanLineOrdering = 1; // PROGRESSIVE
            internal_path.targetSignalInfo.u.videoStandard = 0;
            internal_path.outputTechnology = 5; // HDMI
            internal_path.padding2[17] = 1;
            internal_path.sourceWidth = c.proc.display_width;
            internal_path.sourceHeight = c.proc.display_height;

            static_assert(sizeof(internal_path) == 216);
            c.emu.write_memory(path_array, &internal_path, static_cast<size_t>(path_size));

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
                    utils::string::copy(adapterName.adapterDevicePath, emulated_display::interface_path);
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
                        block.VendorID = emulated_display::vendor_id;
                        block.DeviceID = emulated_display::device_id;
                        block.SubSystemVendorID = emulated_display::vendor_id;
                        block.SubSystemID = 0;
                        block.RevisionID = emulated_display::revision_id;
                        block.WddmVersion = 3200;

                        utils::string::copy(block.AdapterDesc, emulated_display::description);
                        utils::string::copy(block.AdapterDevicePath, emulated_display::interface_path);
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

                display_info.access([&](EMU_GET_DISPLAY_INFO_EX& info) {
                    const bool known_adapter =
                        info.adapterId.LowPart == 0x1000 && info.adapterId.HighPart == 0; // Must match k_dxgk_adapter_luid!
                    const bool known_source = info.sourceId == 0;

                    info.FailurePoint = 0;

                    if (!known_adapter || !known_source)
                    {
                        return;
                    }

                    info.VendorID = emulated_display::vendor_id;
                    info.DeviceID = emulated_display::device_id;
                    info.SubSysID0 = 0;
                    info.SubSysID1 = 0;
                    info.RevisionID = emulated_display::revision_id;
                    info.WddmVersion = 2700;

                    utils::string::copy(info.AdapterDesc, emulated_display::description);

                    info.DisplayLeft = 0;
                    info.DisplayTop = 0;
                    info.DisplayWidth = static_cast<INT32>(c.proc.display_width);
                    info.DisplayHeight = static_cast<INT32>(c.proc.display_height);

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
            c.vcpu.active_thread->clear_queue_status_changed(flags);
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

        BOOL handle_NtUserSetMenu(const syscall_context& c, const hwnd hwnd, const hmenu menu, const BOOL redraw)
        {
            auto* win = c.proc.windows.get(hwnd);
            if (!win)
            {
                set_guest_last_error(c, 1400);
                return FALSE;
            }

            emulator_pointer menu_ptr = 0;
            if (menu != 0)
            {
                const auto* menu_obj = c.proc.menus.get(menu);
                if (!menu_obj)
                {
                    set_guest_last_error(c, 1401);
                    return FALSE;
                }

                menu_ptr = menu_obj->guest.value();
            }

            win->guest.access([&](USER_WINDOW& guest_win) { guest_win.spmenu = menu_ptr; });

            if (redraw != FALSE)
            {
                invalidate_window(c, *win);
            }

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
            std::unordered_set<hmenu> visited;
            return destroy_menu_tree(c, menu, visited) ? TRUE : FALSE;
        }

        BOOL handle_NtUserDrawMenuBar(const syscall_context& c, const hwnd hwnd)
        {
            if (hwnd != 0 && !c.proc.windows.get(hwnd))
            {
                return FALSE;
            }

            return TRUE;
        }

        int32_t handle_NtUserEnableMenuItem(const syscall_context& c, const hmenu menu, const UINT item, const UINT enable)
        {
            auto* menu_object = c.proc.menus.get(menu);
            if (!menu_object)
            {
                return -1;
            }

            size_t index{};
            if ((enable & MF_BYPOSITION) != 0)
            {
                if (item >= menu_object->items.size())
                {
                    return -1;
                }

                index = item;
            }
            else
            {
                const auto entry =
                    std::ranges::find_if(menu_object->items, [&](const menu_item& candidate) { return candidate.id == item; });

                if (entry == menu_object->items.end())
                {
                    return -1;
                }

                index = static_cast<size_t>(std::distance(menu_object->items.begin(), entry));
            }

            constexpr UINT state_mask = MF_DISABLED | MF_GRAYED;

            auto& menu_item = menu_object->items[index];
            const UINT previous_state = menu_item.state & state_mask;

            menu_item.state = (menu_item.state & ~state_mask) | (enable & state_mask);

            menu_object->sync_guest_item(c.win_emu.memory, index);
            return static_cast<int32_t>(previous_state);
        }

        int32_t handle_NtUserCheckMenuItem(const syscall_context& c, const hmenu menu, const UINT item, const UINT check)
        {
            auto* menu_object = c.proc.menus.get(menu);
            if (!menu_object)
            {
                return -1;
            }

            size_t index{};
            if ((check & MF_BYPOSITION) != 0)
            {
                if (item >= menu_object->items.size())
                {
                    return -1;
                }

                index = item;
            }
            else
            {
                std::vector<hmenu> pending{menu};
                std::unordered_set<hmenu> visited{};
                bool found = false;

                while (!pending.empty() && !found)
                {
                    const hmenu current_handle = pending.back();
                    pending.pop_back();
                    if (!visited.insert(current_handle).second)
                    {
                        continue;
                    }

                    auto* current_menu = c.proc.menus.get(current_handle);
                    if (!current_menu)
                    {
                        continue;
                    }

                    for (size_t current_index = 0; current_index < current_menu->items.size(); ++current_index)
                    {
                        const auto& candidate = current_menu->items[current_index];
                        if (candidate.submenu != 0)
                        {
                            pending.push_back(candidate.submenu);
                        }
                        else if (candidate.id == item)
                        {
                            menu_object = current_menu;
                            index = current_index;
                            found = true;
                            break;
                        }
                    }
                }

                if (!found)
                {
                    return -1;
                }
            }

            auto& menu_item = menu_object->items[index];
            const UINT previous_state = menu_item.state & MF_CHECKED;
            menu_item.state = (menu_item.state & ~MF_CHECKED) | (check & MF_CHECKED);
            menu_object->sync_guest_item(c.win_emu.memory, index);
            return static_cast<int32_t>(previous_state);
        }

        BOOL handle_NtUserCreateCaret()
        {
            return TRUE;
        }

        BOOL handle_NtUserDestroyCaret()
        {
            return TRUE;
        }

        BOOL handle_NtUserGetCaretPos(const syscall_context&, const emulator_object<POINT> point)
        {
            if (point)
            {
                point.write(POINT{});
            }
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

        BOOL handle_NtUserGetObjectInformation(const syscall_context& c, const handle object, const int index,
                                               const emulator_pointer information, const DWORD length, const emulator_object<DWORD> needed)
        {
            constexpr uint32_t error_invalid_handle = 6;
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_insufficient_buffer = 122;
            constexpr uint32_t error_no_access = 998;
            const auto fail = [&](uint32_t error) {
                set_guest_last_error(c, error);
                return FALSE;
            };
            const bool window_station = is_window_station(c, object);
            const auto* desktop_object = c.proc.desktops.get(object);
            if (!window_station && !desktop_object)
            {
                return fail(error_invalid_handle);
            }
            const auto write = [&](const void* data, DWORD size) {
                if (needed && !needed.try_write(size))
                {
                    return fail(error_no_access);
                }
                if (length < size)
                {
                    return fail(error_insufficient_buffer);
                }
                if (!information || !c.win_emu.memory.try_write_memory(information, data, size))
                {
                    return fail(error_no_access);
                }
                return TRUE;
            };
            switch (index)
            {
            case 1: {
                struct user_object_flags
                {
                    BOOL inherit;
                    BOOL reserved;
                    DWORD flags;
                };

                // WSF_VISIBLE, which only the interactive station has. A process moved to a noninteractive station
                // reads 0 here, which is how .NET's Environment.UserInteractive decides.
                constexpr uint32_t wsf_visible = 1;
                const auto inheritable =
                    window_station ? c.proc.window_stations.attributes_of(object).inherit : c.proc.desktops.attributes_of(object).inherit;
                const user_object_flags flags{.inherit = inheritable,
                                              .reserved = FALSE,
                                              .flags = (window_station && object.bits == interactive_window_station) ? wsf_visible : 0u};
                return write(&flags, sizeof(flags));
            }
            case 2:
            case 3: {
                std::u16string_view name = u"WinSta0";
                if (const auto* station = c.proc.window_stations.get(object))
                {
                    name = station->name;
                }
                else if (!window_station)
                {
                    name = desktop_object->name;
                }
                if (index == 3)
                {
                    name = window_station ? u"WindowStation" : u"Desktop";
                }
                return write(name.data(), static_cast<DWORD>((name.size() + 1) * sizeof(char16_t)));
            }
            case 4:
                return write(c.proc.sid.data(), static_cast<DWORD>(c.proc.sid.size()));
            case 6: {
                if (window_station)
                {
                    return fail(error_invalid_parameter);
                }
                const BOOL receives_input = resolve_desktop(c, object) == c.proc.default_desktop;
                return write(&receives_input, sizeof(receives_input));
            }
            default:
                return fail(error_invalid_parameter);
            }
        }

        BOOL handle_NtUserGetWindowDisplayAffinity(const syscall_context& c, const hwnd window, const emulator_object<DWORD> affinity)
        {
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_no_access = 998;
            constexpr uint32_t error_invalid_window_handle = 1400;
            constexpr DWORD wda_none = 0;

            const auto* win = c.proc.windows.get(window);
            if (!win)
            {
                set_guest_last_error(c, error_invalid_window_handle);
                return FALSE;
            }

            if (win->parent_handle != c.proc.default_desktop_window_handle.bits)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return FALSE;
            }

            if (!affinity || !affinity.try_write(wda_none))
            {
                set_guest_last_error(c, error_no_access);
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserShowOwnedPopups(const syscall_context& c, const hwnd owner, const BOOL show)
        {
            constexpr uint32_t error_invalid_window_handle = 1400;

            if (!c.proc.windows.get(owner))
            {
                set_guest_last_error(c, error_invalid_window_handle);
                return FALSE;
            }

            const auto has_visible_popup = std::ranges::any_of(c.proc.windows, [&](const auto& entry) {
                return entry.second.owner_handle == owner && (entry.second.style & WS_VISIBLE) != 0;
            });

            if (!show && has_visible_popup)
            {
                c.win_emu.log.error("NtUserShowOwnedPopups: hiding visible owned popups is not supported\n");
                c.emu.stop();
                return FALSE;
            }

            return TRUE;
        }

        BOOL handle_NtUserGetCurrentInputMessageSource(const syscall_context& c, const emulator_pointer source)
        {
            struct input_message_source
            {
                uint32_t device_type;
                uint32_t origin_id;
            };

            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_no_access = 998;
            constexpr input_message_source unavailable{.device_type = 0, .origin_id = 0};

            if (source == 0)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return FALSE;
            }

            if (!c.win_emu.memory.try_write_memory(source, &unavailable, sizeof(unavailable)))
            {
                set_guest_last_error(c, error_no_access);
                return FALSE;
            }

            return TRUE;
        }

        DWORD handle_NtUserGetGuiResources(const syscall_context& c, const handle process, const DWORD flags)
        {
            constexpr uint32_t error_invalid_parameter = 87;

            const auto counts_this_process = process.bits == gui_resources_global_process || c.proc.is_current_process_handle(process);
            const auto* other_process = counts_this_process ? nullptr : c.proc.processes.get(process);
            const auto is_other_process = !counts_this_process && (process == STEAM_PROCESS_HANDLE || process == PARENT_PROCESS_HANDLE ||
                                                                   (other_process != nullptr && other_process->process));

            if (flags > gui_resources_userobjects_peak || (!counts_this_process && !is_other_process))
            {
                set_guest_last_error(c, error_invalid_parameter);
                return 0;
            }

            // The GDI and USER objects of another process are not visible to this emulator instance, while Windows
            // reports that process's counts.
            if (is_other_process)
            {
                return 0;
            }

            switch (flags)
            {
            case gui_resources_gdiobjects:
            case gui_resources_gdiobjects_peak: {
                const auto gdi_objects = count_process_gdi_objects(c);
                c.proc.gdi_objects_peak = std::max(c.proc.gdi_objects_peak, gdi_objects);
                return flags == gui_resources_gdiobjects ? gdi_objects : c.proc.gdi_objects_peak;
            }
            case gui_resources_userobjects:
            case gui_resources_userobjects_peak: {
                const auto windows = std::ranges::count_if(c.proc.windows, [&](const auto& entry) {
                    return entry.second.thread_id != 0 && entry.second.handle != c.proc.default_desktop_window_handle.bits;
                });
                const auto user_objects =
                    static_cast<uint32_t>(static_cast<size_t>(windows) + c.proc.menus.size() + c.proc.accelerator_tables.size());
                c.proc.user_objects_peak = std::max(c.proc.user_objects_peak, user_objects);
                return flags == gui_resources_userobjects ? user_objects : c.proc.user_objects_peak;
            }
            default:
                return 0;
            }
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
                return c.proc.process_id;
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

        uint64_t handle_NtUserGetTopLevelWindow(const syscall_context& c, const hwnd window)
        {
            return handle_NtUserGetAncestor(c, window, 2);
        }

        BOOL handle_NtUserGetWindowPlacement(const syscall_context& c, const hwnd window_handle, const emulator_pointer placement_address)
        {
            if (placement_address == 0)
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return FALSE;
            }

            DWORD requested_length{};
            if (!c.win_emu.memory.try_read_memory(placement_address, &requested_length, sizeof(requested_length)))
            {
                set_guest_last_error(c, 998); // ERROR_NOACCESS
                return FALSE;
            }

            if (requested_length != sizeof(EMU_WINDOWPLACEMENT))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return FALSE;
            }

            const auto* win = c.proc.windows.get(window_handle);
            if (!win)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return FALSE;
            }

            EMU_WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            placement.flags = 0;

            if ((win->style & WS_MINIMIZE) != 0)
            {
                placement.showCmd = SW_SHOWMINIMIZED;
            }
            else if ((win->style & WS_MAXIMIZE) != 0)
            {
                placement.showCmd = SW_SHOWMAXIMIZED;
            }
            else
            {
                placement.showCmd = SW_SHOWNORMAL;
            }

            // TODO: The emulator does not currently maintain separate minimized and
            //       maximized placement coordinates.
            placement.ptMinPosition = {.x = -1, .y = -1};
            placement.ptMaxPosition = {.x = -1, .y = -1};
            placement.rcNormalPosition = get_window_rect(*win);

            if (!c.win_emu.memory.try_write_memory(placement_address, &placement, sizeof(placement)))
            {
                set_guest_last_error(c, 998); // ERROR_NOACCESS
                return FALSE;
            }

            return TRUE;
        }

        int32_t get_system_metric(const syscall_context& c, const size_t index)
        {
            return c.proc.user_handles.get_server_info().read().systemMetrics[index];
        }

        RECT get_title_bar_rect(const syscall_context& c, const window& win)
        {
            constexpr size_t sm_cycaption = 4;
            constexpr size_t sm_cxsize = 30;
            constexpr size_t sm_cysmcaption = 51;

            if ((win.style & WS_CAPTION) != WS_CAPTION)
            {
                return {};
            }

            const auto insets = win.nonclient_insets();
            auto rect = get_window_rect(win);
            rect.left += insets.left;
            rect.top += insets.top;
            rect.right -= insets.right;

            const bool tool_window = (win.ex_style & WS_EX_TOOLWINDOW) != 0;
            rect.bottom = rect.top + get_system_metric(c, tool_window ? sm_cysmcaption : sm_cycaption);

            if (!tool_window && (win.style & WS_SYSMENU) != 0)
            {
                rect.left += get_system_metric(c, sm_cxsize);
            }

            return rect;
        }

        std::array<DWORD, 6> get_title_bar_states(const syscall_context& c, const window& win)
        {
            constexpr DWORD state_unavailable = 0x1;
            constexpr DWORD state_invisible = 0x8000;
            constexpr DWORD state_focusable = 0x100000;
            constexpr uint32_t cs_noclose = 0x200;

            enum class title_bar_element : size_t
            {
                title_bar,
                reserved,
                minimize_button,
                maximize_button,
                help_button,
                close_button,
            };

            std::array<DWORD, 6> states{};
            const auto state = [&states](const title_bar_element element) -> DWORD& { return states[static_cast<size_t>(element)]; };
            state(title_bar_element::title_bar) = state_focusable;

            if ((win.style & WS_CAPTION) != WS_CAPTION)
            {
                state(title_bar_element::title_bar) |= state_invisible;
                return states;
            }

            state(title_bar_element::reserved) = state_invisible;

            if ((win.style & WS_SYSMENU) == 0)
            {
                std::fill(states.begin() + static_cast<size_t>(title_bar_element::minimize_button), states.end(), state_invisible);
                return states;
            }

            if ((win.style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) == 0)
            {
                state(title_bar_element::minimize_button) = state_invisible;
                state(title_bar_element::maximize_button) = state_invisible;
            }
            else
            {
                state(title_bar_element::minimize_button) = (win.style & WS_MINIMIZEBOX) == 0 ? state_unavailable : 0;
                state(title_bar_element::maximize_button) = (win.style & WS_MAXIMIZEBOX) == 0 ? state_unavailable : 0;
            }

            if ((win.ex_style & WS_EX_CONTEXTHELP) == 0)
            {
                state(title_bar_element::help_button) = state_invisible;
            }

            const auto window_class = c.proc.classes.find(win.class_key);
            if (window_class != c.proc.classes.end() && (window_class->second.wnd_class.style & cs_noclose) != 0)
            {
                state(title_bar_element::close_button) = state_unavailable;
            }

            return states;
        }

        BOOL handle_NtUserGetTitleBarInfo(const syscall_context& c, const hwnd window_handle, const emulator_pointer info_address)
        {
            const auto* win = c.proc.windows.get(window_handle);
            if (!win)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return FALSE;
            }

            DWORD requested_size{};
            if (info_address == 0 || !c.win_emu.memory.try_read_memory(info_address, &requested_size, sizeof(requested_size)))
            {
                set_guest_last_error(c, 998); // ERROR_NOACCESS
                return FALSE;
            }

            if (requested_size != sizeof(EMU_TITLEBARINFO))
            {
                set_guest_last_error(c, 87); // ERROR_INVALID_PARAMETER
                return FALSE;
            }

            EMU_TITLEBARINFO info{};
            info.cbSize = requested_size;
            info.rcTitleBar = get_title_bar_rect(c, *win);
            const auto states = get_title_bar_states(c, *win);
            std::ranges::copy(states, std::begin(info.rgstate));

            if (!c.win_emu.memory.try_write_memory(info_address, &info, sizeof(info)))
            {
                set_guest_last_error(c, 998); // ERROR_NOACCESS
                return FALSE;
            }

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

        hwnd handle_NtUserChildWindowFromPointEx(const syscall_context& c, const hwnd parent, const uint64_t point, const uint32_t flags)
        {
            constexpr uint32_t cwp_skip_invisible = 1;
            constexpr uint32_t cwp_skip_disabled = 2;
            constexpr uint32_t cwp_skip_transparent = 4;

            const auto* parent_window = c.proc.windows.get(parent);
            if (!parent_window)
            {
                return 0;
            }

            const auto x = static_cast<int32_t>(point);
            const auto y = static_cast<int32_t>(point >> 32);
            if (x < 0 || y < 0 || x >= parent_window->client_width() || y >= parent_window->client_height())
            {
                return 0;
            }

            // The guest's child list runs from the top of the Z order down, so the first match is the one in front.
            uint64_t next = 0;
            parent_window->guest.access([&](const USER_WINDOW& guest_parent) { next = guest_parent.spwndChild; });
            for (size_t guard = 0; next != 0 && guard < c.proc.windows.size(); ++guard)
            {
                USER_WINDOW guest_child{};
                c.emu.read_memory(next, &guest_child, sizeof(guest_child));
                next = guest_child.spwndNext;

                const auto* child = c.proc.windows.get(guest_child.hWnd);
                if (!child || ((flags & cwp_skip_invisible) && !(child->style & WS_VISIBLE)) ||
                    ((flags & cwp_skip_disabled) && (child->style & WS_DISABLED)) ||
                    ((flags & cwp_skip_transparent) && (child->ex_style & WS_EX_TRANSPARENT)))
                {
                    continue;
                }

                if (x >= child->x && x < child->x + child->width && y >= child->y && y < child->y + child->height)
                {
                    return child->handle;
                }
            }

            return parent;
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

        uint32_t handle_NtUserGetCaretBlinkTime()
        {
            return 530;
        }

        BOOL handle_NtUserFlashWindowEx()
        {
            return FALSE;
        }

        BOOL handle_NtUserModifyWindowTouchCapability()
        {
            return TRUE;
        }

        // Enabling swaps the legacy WM_MOUSE* messages for WM_POINTER ones, and nothing here produces WM_POINTER,
        // so that request is refused deliberately, although Windows would accept it. Disabling asks for the mode a
        // process is already in, and succeeds as it does on Windows.
        BOOL handle_NtUserEnableMouseInPointer(const syscall_context& /*c*/, const BOOL enable)
        {
            return enable ? FALSE : TRUE;
        }

        namespace
        {
            namespace spi_action
            {
                constexpr uint32_t getmouse = 0x0003;
                constexpr uint32_t getgridgranularity = 0x0012;
                constexpr uint32_t geticontitlelogfont = 0x001F;
                constexpr uint32_t getnonclientmetrics = 0x0029;
                constexpr uint32_t getminimizedmetrics = 0x002B;
                constexpr uint32_t geticonmetrics = 0x002D;
                constexpr uint32_t getworkarea = 0x0030;
                constexpr uint32_t getfilterkeys = 0x0032;
                constexpr uint32_t gettogglekeys = 0x0034;
                constexpr uint32_t getmousekeys = 0x0036;
                constexpr uint32_t getstickykeys = 0x003A;
                constexpr uint32_t getaccesstimeout = 0x003C;
                constexpr uint32_t getsoundsentry = 0x0040;
                constexpr uint32_t gethighcontrast = 0x0042;
                constexpr uint32_t getanimation = 0x0048;
                constexpr uint32_t getdeskwallpaper = 0x0073;
                constexpr uint32_t getlogicaldpioverride = 0x009E;
                constexpr uint32_t getmenurect = 0x00A2;
                constexpr uint32_t getbeep = 0x0001;
                constexpr uint32_t getborder = 0x0005;
                constexpr uint32_t getkeyboardspeed = 0x000A;
                constexpr uint32_t getscreensavetimeout = 0x000E;
                constexpr uint32_t getscreensaveactive = 0x0010;
                constexpr uint32_t getkeyboarddelay = 0x0016;
                constexpr uint32_t iconhorizontalspacing = 0x000D;
                constexpr uint32_t iconverticalspacing = 0x0018;
                constexpr uint32_t geticontitlewrap = 0x0019;
                constexpr uint32_t getmenudropalignment = 0x001B;
                constexpr uint32_t getfasttaskswitch = 0x0023;
                constexpr uint32_t getdragfullwindows = 0x0026;
                constexpr uint32_t getkeyboardcues = 0x100A;
                constexpr uint32_t getkeyboardpref = 0x0044;
                constexpr uint32_t getscreenreader = 0x0046;
                constexpr uint32_t getfontsmoothing = 0x004A;
                constexpr uint32_t getlowpowertimeout = 0x004F;
                constexpr uint32_t getpowerofftimeout = 0x0050;
                constexpr uint32_t getlowpoweractive = 0x0053;
                constexpr uint32_t getpoweroffactive = 0x0054;
                constexpr uint32_t getsnaptodefbutton = 0x005F;
                constexpr uint32_t getmousehoverwidth = 0x0062;
                constexpr uint32_t getmousehoverheight = 0x0064;
                constexpr uint32_t getmousehovertime = 0x0066;
                constexpr uint32_t getwheelscrolllines = 0x0068;
                constexpr uint32_t getmenushowdelay = 0x006A;
                constexpr uint32_t getwheelscrollchars = 0x006C;
                constexpr uint32_t getshowimeui = 0x006E;
                constexpr uint32_t getmousespeed = 0x0070;
                constexpr uint32_t getscreensaverrunning = 0x0072;
                constexpr uint32_t getactivewindowtracking = 0x1000;
                constexpr uint32_t getmenuanimation = 0x1002;
                constexpr uint32_t getcomboboxanimation = 0x1004;
                constexpr uint32_t getlistboxsmoothscrolling = 0x1006;
                constexpr uint32_t getgradientcaptions = 0x1008;
                constexpr uint32_t getactivewndtrkzorder = 0x100C;
                constexpr uint32_t gethottracking = 0x100E;
                constexpr uint32_t getmenufade = 0x1012;
                constexpr uint32_t getselectionfade = 0x1014;
                constexpr uint32_t gettooltipanimation = 0x1016;
                constexpr uint32_t gettooltipfade = 0x1018;
                constexpr uint32_t getcursorshadow = 0x101A;
                constexpr uint32_t getuieffects = 0x103E;
                constexpr uint32_t getdisableoverlappedcontent = 0x1040;
                constexpr uint32_t getclientareaanimation = 0x1042;
                constexpr uint32_t getcleartype = 0x1048;
                constexpr uint32_t getspeechrecognition = 0x104A;
                constexpr uint32_t getforegroundlocktimeout = 0x2000;
                constexpr uint32_t getactivewndtrktimeout = 0x2002;
                constexpr uint32_t getforegroundflashcount = 0x2004;
                constexpr uint32_t getcaretwidth = 0x2006;
                constexpr uint32_t getmouseclicklocktime = 0x2008;
                constexpr uint32_t getfontsmoothingtype = 0x200A;
                constexpr uint32_t getfontsmoothingcontrast = 0x200C;
                constexpr uint32_t getfocusborderwidth = 0x200E;
                constexpr uint32_t getfocusborderheight = 0x2010;
                constexpr uint32_t getfontsmoothingorientation = 0x2012;
                constexpr uint32_t getmousewheelrouting = 0x201C;
                constexpr uint32_t getthreadlocalinputsettings = 0x104E;
                constexpr uint32_t getsystemlanguagebar = 0x1050;
                constexpr uint32_t getmessageduration = 0x2016;
                constexpr uint32_t getcontactvisualization = 0x2018;
                constexpr uint32_t getgesturevisualization = 0x201A;
                constexpr uint32_t getwinarranging = 0x0082;
                constexpr uint32_t getdockmoving = 0x0090;
                constexpr uint32_t getsnapsizing = 0x008E;
                constexpr uint32_t getflatmenu = 0x1022;
                constexpr uint32_t getdropshadow = 0x1024;
                constexpr uint32_t getblocksendinputresets = 0x1026;
                constexpr uint32_t getshowsounds = 0x0038;
                constexpr uint32_t getserialkeys = 0x003E;
                constexpr uint32_t getdefaultinputlang = 0x0059;
                constexpr uint32_t getwindowsextension = 0x005C;
                constexpr uint32_t getmousetrails = 0x005E;
                constexpr uint32_t getaudiodescription = 0x0074;
                constexpr uint32_t getscreensavesecure = 0x0076;
                constexpr uint32_t gethungapptimeout = 0x0078;
                constexpr uint32_t getwaittokilltimeout = 0x007A;
                constexpr uint32_t getwaittokillservicetimeout = 0x007C;
                constexpr uint32_t getmousedockthreshold = 0x007E;
                constexpr uint32_t getpendockthreshold = 0x0080;
                constexpr uint32_t getmousedragoutthreshold = 0x0084;
                constexpr uint32_t getpendragoutthreshold = 0x0086;
                constexpr uint32_t getmousesidemovethreshold = 0x0088;
                constexpr uint32_t getpensidemovethreshold = 0x008A;
                constexpr uint32_t getdragfrommaximize = 0x008C;
                constexpr uint32_t gettouchpredictionparameters = 0x009C;
                constexpr uint32_t getmousecornercliplength = 0x00A0;
                constexpr uint32_t getmousesonar = 0x101C;
                constexpr uint32_t getmouseclicklock = 0x101E;
                constexpr uint32_t getmousevanish = 0x1020;
                constexpr uint32_t getcaretbrowsing = 0x104C;
                constexpr uint32_t getminimumhitradius = 0x2014;
                constexpr uint32_t getpenvisualization = 0x201E;
                constexpr uint32_t getpenarbitrationtype = 0x2020;
                constexpr uint32_t getcarettimeout = 0x2022;
                constexpr uint32_t gethandedness = 0x2024;
            }

            struct spi_scalar
            {
                uint32_t action;
                uint32_t value;
            };

            constexpr auto k_scalar_gets = std::to_array<spi_scalar>({
                {.action = spi_action::getbeep, .value = 1},
                {.action = spi_action::getborder, .value = 1},
                {.action = spi_action::getkeyboardspeed, .value = 31},
                {.action = spi_action::getscreensavetimeout, .value = 0},
                {.action = spi_action::getscreensaveactive, .value = 1},
                {.action = spi_action::getkeyboarddelay, .value = 1},
                {.action = spi_action::iconhorizontalspacing, .value = 75},
                {.action = spi_action::iconverticalspacing, .value = 75},
                {.action = spi_action::geticontitlewrap, .value = 1},
                {.action = spi_action::getmenudropalignment, .value = 0},
                {.action = spi_action::getfasttaskswitch, .value = 1},
                {.action = spi_action::getdragfullwindows, .value = 1},
                {.action = spi_action::getkeyboardcues, .value = 0},
                {.action = spi_action::getkeyboardpref, .value = 0},
                {.action = spi_action::getscreenreader, .value = 0},
                {.action = spi_action::getfontsmoothing, .value = 1},
                {.action = spi_action::getlowpowertimeout, .value = 0},
                {.action = spi_action::getpowerofftimeout, .value = 0},
                {.action = spi_action::getlowpoweractive, .value = 0},
                {.action = spi_action::getpoweroffactive, .value = 0},
                {.action = spi_action::getsnaptodefbutton, .value = 0},
                {.action = spi_action::getmousehoverwidth, .value = 4},
                {.action = spi_action::getmousehoverheight, .value = 4},
                {.action = spi_action::getmousehovertime, .value = 400},
                {.action = spi_action::getwheelscrolllines, .value = 3},
                {.action = spi_action::getmenushowdelay, .value = 400},
                {.action = spi_action::getwheelscrollchars, .value = 3},
                {.action = spi_action::getshowimeui, .value = 1},
                {.action = spi_action::getmousespeed, .value = 10},
                {.action = spi_action::getscreensaverrunning, .value = 0},
                {.action = spi_action::getactivewindowtracking, .value = 0},
                {.action = spi_action::getmenuanimation, .value = 1},
                {.action = spi_action::getcomboboxanimation, .value = 1},
                {.action = spi_action::getlistboxsmoothscrolling, .value = 1},
                {.action = spi_action::getgradientcaptions, .value = 1},
                {.action = spi_action::getactivewndtrkzorder, .value = 0},
                {.action = spi_action::gethottracking, .value = 1},
                {.action = spi_action::getmenufade, .value = 1},
                {.action = spi_action::getselectionfade, .value = 1},
                {.action = spi_action::gettooltipanimation, .value = 1},
                {.action = spi_action::gettooltipfade, .value = 1},
                {.action = spi_action::getcursorshadow, .value = 1},
                {.action = spi_action::getuieffects, .value = 1},
                {.action = spi_action::getdisableoverlappedcontent, .value = 0},
                {.action = spi_action::getclientareaanimation, .value = 1},
                {.action = spi_action::getcleartype, .value = 1},
                {.action = spi_action::getspeechrecognition, .value = 0},
                {.action = spi_action::getforegroundlocktimeout, .value = 200000},
                {.action = spi_action::getactivewndtrktimeout, .value = 0},
                {.action = spi_action::getforegroundflashcount, .value = 7},
                {.action = spi_action::getcaretwidth, .value = 1},
                {.action = spi_action::getmouseclicklocktime, .value = 1200},
                {.action = spi_action::getfontsmoothingtype, .value = 2},
                {.action = spi_action::getfontsmoothingcontrast, .value = 1200},
                {.action = spi_action::getfocusborderwidth, .value = 1},
                {.action = spi_action::getfocusborderheight, .value = 1},
                {.action = spi_action::getfontsmoothingorientation, .value = 1},
                {.action = spi_action::getmousewheelrouting, .value = 2},
                {.action = spi_action::getthreadlocalinputsettings, .value = 0},
                {.action = spi_action::getsystemlanguagebar, .value = 0},
                {.action = spi_action::getmessageduration, .value = 5},
                {.action = spi_action::getcontactvisualization, .value = 0},
                {.action = spi_action::getgesturevisualization, .value = 24},
                {.action = spi_action::getwinarranging, .value = 1},
                {.action = spi_action::getdockmoving, .value = 1},
                {.action = spi_action::getsnapsizing, .value = 1},
                {.action = spi_action::getflatmenu, .value = 1},
                {.action = spi_action::getdropshadow, .value = 0},
                {.action = spi_action::getblocksendinputresets, .value = 0},
                {.action = spi_action::getshowsounds, .value = 0},
                {.action = spi_action::getmousetrails, .value = 0},
                {.action = spi_action::getscreensavesecure, .value = 0},
                {.action = spi_action::gethungapptimeout, .value = 5000},
                {.action = spi_action::getwaittokilltimeout, .value = 5000},
                {.action = spi_action::getwaittokillservicetimeout, .value = 5000},
                {.action = spi_action::getmousedockthreshold, .value = 1},
                {.action = spi_action::getpendockthreshold, .value = 30},
                {.action = spi_action::getmousedragoutthreshold, .value = 20},
                {.action = spi_action::getpendragoutthreshold, .value = 30},
                {.action = spi_action::getmousesidemovethreshold, .value = 50},
                {.action = spi_action::getpensidemovethreshold, .value = 50},
                {.action = spi_action::getdragfrommaximize, .value = 1},
                {.action = spi_action::getmousecornercliplength, .value = 6},
                {.action = spi_action::getmousesonar, .value = 0},
                {.action = spi_action::getmouseclicklock, .value = 0},
                {.action = spi_action::getmousevanish, .value = 1},
                {.action = spi_action::getcaretbrowsing, .value = 0},
                {.action = spi_action::getminimumhitradius, .value = 0},
                {.action = spi_action::getpenvisualization, .value = 0x23},
                {.action = spi_action::getpenarbitrationtype, .value = 3},
                {.action = spi_action::getcarettimeout, .value = 5000},
                {.action = spi_action::gethandedness, .value = 0},
                // Actions winuser.h does not name, which Windows answers all the same.
                {.action = 0x1010, .value = 0},
                {.action = 0x1028, .value = 0},
                {.action = 0x102A, .value = 0},
                {.action = 0x102C, .value = 0},
                {.action = 0x102E, .value = 0},
                {.action = 0x1030, .value = 0},
                {.action = 0x1032, .value = 0},
                {.action = 0x1034, .value = 0},
                {.action = 0x1036, .value = 0},
                {.action = 0x1038, .value = 0},
                {.action = 0x103A, .value = 0},
                {.action = 0x103C, .value = 0},
                {.action = 0x1044, .value = 0},
                {.action = 0x1046, .value = 0},
                {.action = 0x1052, .value = 0},
                {.action = 0x1054, .value = 0},
                {.action = 0x2026, .value = 0},
                {.action = 0x2028, .value = 0x20},
                {.action = 0x202A, .value = 0},
                {.action = 0x202C, .value = 0},
                {.action = 0x202E, .value = 0},
                {.action = 0x2030, .value = 1},
                {.action = 0x2032, .value = 1},
                {.action = 0x2034, .value = 0},
                {.action = 0x2036, .value = 0},
                {.action = 0x2038, .value = 0},
            });

            constexpr std::array<uint32_t, 3> k_mouse_acceleration{6, 10, 1};
            constexpr std::array<uint32_t, 5> k_minimized_metrics{20, 154, 0, 0, 0};
            constexpr DWORD k_high_contrast_flags = 0x7E;
            constexpr uint32_t k_default_dpi = 96;
            constexpr DWORD k_error_success = 0;
            constexpr DWORD k_error_invalid_parameter = 87;
            constexpr DWORD k_error_noaccess = 998;
            constexpr uint16_t k_flat_border_subsystem_version = 6;
            constexpr DWORD k_error_invalid_spi_value = 1439;
            constexpr uint32_t k_default_input_language = 0x04090409;
            constexpr uint32_t k_touch_prediction_parameters_size = 16;
            constexpr std::array<uint32_t, 3> k_touch_prediction_parameters{8, 8, 1};
            constexpr uint32_t k_max_wallpaper_chars = 260;
            constexpr uint32_t k_update_ini_file = 0x1;
            constexpr uint32_t k_output_alignment = 4;

            constexpr bool rejects_non_zero_param(const uint32_t action)
            {
                const bool in_range = (action >= 0x1000 && action <= 0x1055) || (action >= 0x2000 && action <= 0x2039);
                return in_range && action != spi_action::getblocksendinputresets;
            }

            constexpr bool fails_with_update_ini_file(const uint32_t action)
            {
                switch (action)
                {
                case spi_action::getpenvisualization:
                case spi_action::getpenarbitrationtype:
                case 0x2026:
                case 0x2030:
                case 0x2032:
                case 0x2034:
                case 0x2038:
                    return true;
                default:
                    return false;
                }
            }

            BOOL fail_with(const syscall_context& c, const DWORD error)
            {
                set_guest_last_error(c, error);
                return FALSE;
            }

            // win32kbase probes these outputs at 4 bytes (1 for a WOW64 caller), also the 8-byte HKL.
            bool is_misaligned_output(const syscall_context& c, const emulator_pointer value)
            {
                return !c.proc.is_wow64_process && value % k_output_alignment != 0;
            }

            BOOL write_output(const syscall_context& c, const emulator_pointer value, const void* data, const size_t size)
            {
                if (!is_writable_range(c.win_emu, value, size) || !c.win_emu.memory.try_write_memory(value, data, size))
                {
                    return fail_with(c, k_error_noaccess);
                }

                return TRUE;
            }

            std::optional<uint32_t> probe_output_struct_size(const syscall_context& c, const emulator_pointer value, const size_t size)
            {
                uint32_t struct_size{};
                if (!is_writable_range(c.win_emu, value, size) ||
                    !c.win_emu.memory.try_read_memory(value, &struct_size, sizeof(struct_size)))
                {
                    return std::nullopt;
                }

                return struct_size;
            }

            // Windows copies its zeroed output buffer back even though the call fails.
            bool write_zeroed_output(const syscall_context& c, const emulator_pointer value, const size_t size, const uint32_t struct_size)
            {
                std::vector<std::byte> zeroed(size);
                std::memcpy(zeroed.data(), &struct_size, sizeof(struct_size));
                return c.win_emu.memory.try_write_memory(value, zeroed.data(), zeroed.size());
            }

            DWORD check_for_dpi_output(const syscall_context& c, const emulator_pointer value, const uint32_t param, const size_t size,
                                       const bool has_struct_size)
            {
                if (param != size)
                {
                    return k_error_invalid_parameter;
                }

                if (!has_struct_size)
                {
                    return is_writable_range(c.win_emu, value, size) ? k_error_success : k_error_noaccess;
                }

                const auto struct_size = probe_output_struct_size(c, value, size);
                if (!struct_size)
                {
                    return k_error_noaccess;
                }

                if (*struct_size == size)
                {
                    return k_error_success;
                }

                return write_zeroed_output(c, value, size, *struct_size) ? k_error_invalid_parameter : k_error_noaccess;
            }

            LONG scale_to_dpi(const LONG value, const uint32_t dpi)
            {
                return static_cast<LONG>((static_cast<int64_t>(value) * dpi + k_default_dpi / 2) / k_default_dpi);
            }

            // The metrics and logfonts a Windows 11 desktop reports at 96 DPI.
            void fill_ui_logfont(EMU_LOGFONTW& font, const uint32_t dpi)
            {
                font.lfHeight = -static_cast<LONG>(9ull * dpi / 72ull);
                font.lfWeight = FW_NORMAL;
                font.lfCharSet = DEFAULT_CHARSET;
                std::ranges::copy(u"Segoe UI", font.lfFaceName);
            }

            BOOL write_icontitle_logfont(const syscall_context& c, const emulator_pointer value, const uint32_t dpi)
            {
                EMU_LOGFONTW font{};
                fill_ui_logfont(font, dpi);
                return write_output(c, value, &font, sizeof(font));
            }

            BOOL write_icon_metrics(const syscall_context& c, const emulator_pointer value, const uint32_t dpi)
            {
                EMU_ICONMETRICS metrics{};
                metrics.cbSize = sizeof(metrics);
                metrics.iHorzSpacing = scale_to_dpi(75, dpi);
                metrics.iVertSpacing = scale_to_dpi(75, dpi);
                metrics.iTitleWrap = TRUE;
                fill_ui_logfont(metrics.lfFont, dpi);
                return write_output(c, value, &metrics, sizeof(metrics));
            }

            BOOL write_nonclient_metrics(const syscall_context& c, const emulator_pointer value, const uint32_t size, const uint32_t dpi,
                                         const LONG border_width, const LONG padded_border_width)
            {
                constexpr auto min_size = offsetof(EMU_NONCLIENTMETRICS, lfMessageFont) + sizeof(EMU_LOGFONTW);
                if (size < min_size)
                {
                    return fail_with(c, k_error_invalid_parameter);
                }

                const auto length = std::min<size_t>(size, sizeof(EMU_NONCLIENTMETRICS));

                EMU_NONCLIENTMETRICS metrics{};
                metrics.cbSize = static_cast<uint32_t>(length);
                metrics.iBorderWidth = border_width;
                metrics.iScrollWidth = scale_to_dpi(17, dpi);
                metrics.iScrollHeight = scale_to_dpi(17, dpi);
                metrics.iCaptionWidth = scale_to_dpi(22, dpi);
                metrics.iCaptionHeight = scale_to_dpi(22, dpi);
                metrics.iSmCaptionWidth = scale_to_dpi(22, dpi);
                metrics.iSmCaptionHeight = scale_to_dpi(22, dpi);
                metrics.iMenuWidth = scale_to_dpi(19, dpi);
                metrics.iMenuHeight = scale_to_dpi(19, dpi);
                metrics.iPaddedBorderWidth = padded_border_width;
                fill_ui_logfont(metrics.lfCaptionFont, dpi);
                fill_ui_logfont(metrics.lfSmCaptionFont, dpi);
                fill_ui_logfont(metrics.lfMenuFont, dpi);
                fill_ui_logfont(metrics.lfStatusFont, dpi);
                fill_ui_logfont(metrics.lfMessageFont, dpi);

                return write_output(c, value, &metrics, length);
            }

            BOOL get_primary_work_area(const syscall_context& c, const emulator_pointer value)
            {
                const emulator_object<USER_MONITOR> monitor(c.emu, c.proc.user_handles.get_display_info().read().pPrimaryMonitor);
                if (!monitor)
                {
                    return FALSE;
                }

                const auto work_area = monitor.read().rcWork;
                return write_output(c, value, &work_area, sizeof(work_area));
            }

            BOOL get_animation(const syscall_context& c, const uint32_t param, const emulator_pointer value)
            {
                const bool size_matches = param == sizeof(EMU_ANIMATIONINFO);
                const LONG min_animate = size_matches ? TRUE : FALSE;
                if (!write_output(c, value + offsetof(EMU_ANIMATIONINFO, iMinAnimate), &min_animate, sizeof(min_animate)))
                {
                    return FALSE;
                }

                return size_matches ? TRUE : FALSE;
            }

            BOOL get_high_contrast(const syscall_context& c, const emulator_pointer value)
            {
                return write_output(c, value + offsetof(EMU_HIGHCONTRAST, dwFlags), &k_high_contrast_flags, sizeof(k_high_contrast_flags));
            }

            const std::unordered_map<uint32_t, std::vector<uint32_t>>& sized_struct_gets()
            {
                static const std::unordered_map<uint32_t, std::vector<uint32_t>> structs{
                    {spi_action::getfilterkeys, {24, 0x7E, 1000, 1000, 500, 0}},
                    {spi_action::gettogglekeys, {8, 0x3E}},
                    {spi_action::getmousekeys, {28, 0x1000003E, 80, 3000, 0, 0, 0}},
                    {spi_action::getstickykeys, {8, 0x1FE}},
                    {spi_action::getaccesstimeout, {12, 2, 300000}},
                    {spi_action::getsoundsentry, {56, 2, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}},
                    {spi_action::getaudiodescription, {12, 0, 0}},
                };

                return structs;
            }

            BOOL get_desk_wallpaper(const syscall_context& c, const uint32_t param, const emulator_pointer value)
            {
                if (!is_writable_range(c.win_emu, value, uint64_t{param} * sizeof(char16_t)))
                {
                    return fail_with(c, k_error_noaccess);
                }

                c.win_emu.memory.set_memory(value, 0, uint64_t{std::min(param, k_max_wallpaper_chars)} * sizeof(char16_t));
                return TRUE;
            }

            BOOL get_touch_prediction_parameters(const syscall_context& c, const uint32_t param, const emulator_pointer value)
            {
                if (param != k_touch_prediction_parameters_size)
                {
                    return fail_with(c, k_error_invalid_parameter);
                }

                if (is_misaligned_output(c, value) || !is_writable_range(c.win_emu, value, k_touch_prediction_parameters_size))
                {
                    return fail_with(c, k_error_noaccess);
                }

                return write_output(c, value + sizeof(uint32_t), k_touch_prediction_parameters.data(),
                                    sizeof(k_touch_prediction_parameters));
            }

            BOOL get_setting(const syscall_context& c, const uint32_t action, const uint32_t param, const emulator_pointer value,
                             const uint32_t win_ini)
            {
                for (const auto& scalar : k_scalar_gets)
                {
                    if (scalar.action == action)
                    {
                        if (param != 0 && rejects_non_zero_param(action))
                        {
                            return fail_with(c, k_error_invalid_parameter);
                        }

                        const auto written = write_output(c, value, &scalar.value, sizeof(scalar.value));
                        if (written && (win_ini & k_update_ini_file) != 0 && fails_with_update_ini_file(action))
                        {
                            return FALSE;
                        }

                        return written;
                    }
                }

                const auto& structs = sized_struct_gets();
                if (const auto entry = structs.find(action); entry != structs.end())
                {
                    const auto size = entry->second.size() * sizeof(uint32_t);
                    const auto struct_size = probe_output_struct_size(c, value, size);
                    if (!struct_size)
                    {
                        return fail_with(c, k_error_noaccess);
                    }

                    if (*struct_size == size && (param == 0 || param == size))
                    {
                        return write_output(c, value, entry->second.data(), size);
                    }

                    return write_zeroed_output(c, value, size, *struct_size) ? FALSE : fail_with(c, k_error_noaccess);
                }

                c.win_emu.log.error("Unimplemented NtUserSystemParametersInfo action: 0x%X\n", action);
                return fail_with(c, k_error_invalid_parameter);
            }
        }

        BOOL handle_NtUserSystemParametersInfoForDpi(const syscall_context& c, const uint32_t action, const uint32_t param,
                                                     const emulator_pointer value, const uint32_t /*win_ini*/, const uint32_t dpi)
        {
            if (dpi == 0 || value == 0)
            {
                return fail_with(c, k_error_invalid_parameter);
            }

            switch (action)
            {
            case spi_action::geticontitlelogfont:
                if (const auto error = check_for_dpi_output(c, value, param, sizeof(EMU_LOGFONTW), false))
                {
                    return fail_with(c, error);
                }

                return write_icontitle_logfont(c, value, dpi);
            case spi_action::geticonmetrics:
                if (const auto error = check_for_dpi_output(c, value, param, sizeof(EMU_ICONMETRICS), true))
                {
                    return fail_with(c, error);
                }

                return write_icon_metrics(c, value, dpi);
            case spi_action::getnonclientmetrics:
                if (const auto error = check_for_dpi_output(c, value, param, sizeof(EMU_NONCLIENTMETRICS), true))
                {
                    return fail_with(c, error);
                }

                return write_nonclient_metrics(c, value, param, dpi, scale_to_dpi(1, dpi), scale_to_dpi(4, dpi));
            default:
                return fail_with(c, k_error_invalid_parameter);
            }
        }

        BOOL handle_NtUserSystemParametersInfo(const syscall_context& c, const uint32_t action, const uint32_t param,
                                               const emulator_pointer value, const uint32_t win_ini)
        {
            switch (action)
            {
            case spi_action::getworkarea:
            case spi_action::getmenurect:
                return get_primary_work_area(c, value);
            case spi_action::geticontitlelogfont:
                return write_icontitle_logfont(c, value, k_default_dpi);
            case spi_action::geticonmetrics:
                return write_icon_metrics(c, value, k_default_dpi);
            case spi_action::getnonclientmetrics: {
                const auto flat_borders = c.win_emu.mod_manager.executable->subsystem_major_version >= k_flat_border_subsystem_version;
                return write_nonclient_metrics(c, value, param, k_default_dpi, flat_borders ? 1 : 5, flat_borders ? 4 : 0);
            }
            case spi_action::getanimation:
                return get_animation(c, param, value);
            case spi_action::gethighcontrast:
                return get_high_contrast(c, value);
            case spi_action::getminimizedmetrics:
                return write_output(c, value, k_minimized_metrics.data(), sizeof(k_minimized_metrics));
            case spi_action::getmouse:
                return write_output(c, value, k_mouse_acceleration.data(), sizeof(k_mouse_acceleration));
            case spi_action::getdeskwallpaper:
                return get_desk_wallpaper(c, param, value);
            // Windows reports failure here although it has written the zero.
            case spi_action::getlogicaldpioverride: {
                constexpr int32_t no_override = 0;
                write_output(c, value, &no_override, sizeof(no_override));
                return FALSE;
            }
            case spi_action::getdefaultinputlang: {
                constexpr uint64_t input_language = k_default_input_language;
                if (is_misaligned_output(c, value))
                {
                    return fail_with(c, k_error_noaccess);
                }

                return write_output(c, value, &input_language, sizeof(input_language));
            }
            case spi_action::getaudiodescription:
                if (is_misaligned_output(c, value))
                {
                    return fail_with(c, k_error_noaccess);
                }

                return get_setting(c, action, param, value, win_ini);
            case spi_action::gettouchpredictionparameters:
                return get_touch_prediction_parameters(c, param, value);
            case spi_action::getserialkeys:
            case spi_action::getwindowsextension:
                return fail_with(c, k_error_invalid_parameter);
            case 0:
            case spi_action::getgridgranularity:
                return fail_with(c, k_error_invalid_spi_value);
            default:
                return get_setting(c, action, param, value, win_ini);
            }
        }

        uint64_t handle_NtUserGetProcessDpiAwarenessContext()
        {
            return USER_DEFAULT_DPI_CONTEXT;
        }

        BOOL handle_NtUserSetProcessDpiAwarenessContext()
        {
            return TRUE;
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
                return us_layout_character(code, false, true);
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

        int32_t handle_NtUserToUnicodeEx(const syscall_context& c, const uint32_t virtual_key, const uint32_t scan_code,
                                         const emulator_pointer key_state, const emulator_pointer buffer, const int32_t buffer_size,
                                         const uint32_t flags, const uint64_t /*keyboard_layout*/)
        {
            constexpr uint32_t key_up = 0x8000;
            constexpr uint32_t translate_key_up = 0x2;
            const auto ignored_key_up = (scan_code & key_up) != 0 && (flags & translate_key_up) == 0;
            if (buffer == 0 || buffer_size <= 0 || ignored_key_up || virtual_key >= 0x100)
            {
                return 0;
            }

            std::array<uint8_t, 256> keys{};
            if (key_state != 0 && !c.win_emu.memory.try_read_memory(key_state, keys.data(), keys.size()))
            {
                return 0;
            }

            const auto character = key_character(virtual_key, keys);
            if (character == 0)
            {
                return 0;
            }

            const std::array<char16_t, 2> text{character, u'\0'};
            const auto length = buffer_size > 1 ? text.size() : 1;
            if (!c.win_emu.memory.try_write_memory(buffer, text.data(), length * sizeof(char16_t)))
            {
                return 0;
            }

            return 1;
        }

        struct emu_mouse_input
        {
            int32_t dx;
            int32_t dy;
            uint32_t mouse_data;
            uint32_t flags;
            uint32_t time;
            uint64_t extra_info;
        };

        struct emu_keyboard_input
        {
            uint16_t virtual_key;
            uint16_t scan_code;
            uint32_t flags;
            uint32_t time;
            uint64_t extra_info;
        };

        struct emu_input
        {
            uint32_t type;

            union
            {
                emu_mouse_input mouse;
                emu_keyboard_input keyboard;
            };
        };

        static_assert(sizeof(emu_input) == 40);

        uint32_t generic_virtual_key(const uint32_t virtual_key)
        {
            switch (virtual_key)
            {
            case VK_LSHIFT:
            case VK_RSHIFT:
                return VK_SHIFT;
            case VK_LCONTROL:
            case VK_RCONTROL:
                return VK_CONTROL;
            case VK_LMENU:
            case VK_RMENU:
                return VK_MENU;
            default:
                return virtual_key;
            }
        }

        void send_mouse_input(const syscall_context& c, const emu_mouse_input& input)
        {
            constexpr uint32_t mouseeventf_move = 0x0001;
            constexpr uint32_t mouseeventf_absolute = 0x8000;

            auto& proc = c.proc;
            const auto window = find_foreground_window(c);

            // Like host motion, injected pointer events let the emulator clip the cursor and feed raw input. Button and
            // wheel events stay at wherever the cursor is when they are delivered.
            const auto send = [&](const uint32_t message, const uint64_t w_param, const std::optional<std::pair<int32_t, int32_t>> motion,
                                  const std::optional<std::pair<int32_t, int32_t>> target) {
                auto relative_motion = motion;
                if (!motion && !target)
                {
                    relative_motion = std::pair{0, 0};
                }

                c.win_emu.handle_ui_event_locked(ui_event{
                    .window = window,
                    .message = message,
                    .wParam = w_param,
                    .lParam = 0,
                    .relative_motion = relative_motion,
                    .cursor_target = target,
                    .injected = true,
                    .extra_info = input.extra_info,
                });
            };

            if (input.flags & mouseeventf_move)
            {
                std::optional<std::pair<int32_t, int32_t>> motion{};
                std::optional<std::pair<int32_t, int32_t>> target{};
                if (input.flags & mouseeventf_absolute)
                {
                    // Absolute coordinates are normalized to 0..65535 across the display.
                    target = std::pair{static_cast<int32_t>((static_cast<int64_t>(input.dx) * proc.display_width) >> 16),
                                       static_cast<int32_t>((static_cast<int64_t>(input.dy) * proc.display_height) >> 16)};
                }
                else
                {
                    motion = std::pair{input.dx, input.dy};
                }

                if (window == 0)
                {
                    auto x = target ? target->first : proc.cursor_x + input.dx;
                    auto y = target ? target->second : proc.cursor_y + input.dy;
                    if (const auto& clip = proc.cursor_clip)
                    {
                        const auto left = static_cast<int32_t>(clip->left);
                        const auto top = static_cast<int32_t>(clip->top);
                        x = std::clamp(x, left, std::max(left, static_cast<int32_t>(clip->right) - 1));
                        y = std::clamp(y, top, std::max(top, static_cast<int32_t>(clip->bottom) - 1));
                    }
                    proc.cursor_x = x;
                    proc.cursor_y = y;
                }
                else
                {
                    send(WM_MOUSEMOVE, 0, motion, target);
                }
            }

            const auto press = [&](const uint32_t message, const uint32_t virtual_key, const bool down, const uint64_t w_param) {
                // Without a window to receive it, injected input still changes what GetAsyncKeyState reports.
                if (window == 0)
                {
                    if (down && (proc.key_state[virtual_key] & 0x80) == 0)
                    {
                        proc.async_key_state[virtual_key] = 1;
                    }
                    proc.key_state[virtual_key] = down ? 0x80 : 0;
                    proc.key_state_changed();
                    return;
                }

                send(message, w_param, std::nullopt, std::nullopt);
            };

            struct button_transition
            {
                uint32_t flag;
                uint32_t message;
                uint32_t virtual_key;
                bool down;
            };

            constexpr std::array<button_transition, 6> transitions{{
                {.flag = 0x0002, .message = WM_LBUTTONDOWN, .virtual_key = VK_LBUTTON, .down = true},
                {.flag = 0x0004, .message = WM_LBUTTONUP, .virtual_key = VK_LBUTTON, .down = false},
                {.flag = 0x0008, .message = WM_RBUTTONDOWN, .virtual_key = VK_RBUTTON, .down = true},
                {.flag = 0x0010, .message = WM_RBUTTONUP, .virtual_key = VK_RBUTTON, .down = false},
                {.flag = 0x0020, .message = WM_MBUTTONDOWN, .virtual_key = VK_MBUTTON, .down = true},
                {.flag = 0x0040, .message = WM_MBUTTONUP, .virtual_key = VK_MBUTTON, .down = false},
            }};

            for (const auto& t : transitions)
            {
                if (input.flags & t.flag)
                {
                    press(t.message, t.virtual_key, t.down, 0);
                }
            }

            constexpr uint32_t mouseeventf_xdown = 0x0080;
            constexpr uint32_t mouseeventf_xup = 0x0100;
            for (const uint32_t flag : {mouseeventf_xdown, mouseeventf_xup})
            {
                if ((input.flags & flag) == 0)
                {
                    continue;
                }

                const bool down = flag == mouseeventf_xdown;
                for (const uint32_t button : {XBUTTON1, XBUTTON2})
                {
                    if (input.mouse_data & button)
                    {
                        press(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, button == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, down,
                              static_cast<uint64_t>(button) << 16);
                    }
                }
            }

            constexpr uint32_t mouseeventf_wheel = 0x0800;
            constexpr uint32_t mouseeventf_hwheel = 0x1000;
            if (window != 0 && (input.flags & (mouseeventf_wheel | mouseeventf_hwheel)))
            {
                const auto delta = static_cast<uint16_t>(input.mouse_data);
                send((input.flags & mouseeventf_wheel) ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL, static_cast<uint64_t>(delta) << 16, std::nullopt,
                     std::nullopt);
            }
        }

        void send_keyboard_input(const syscall_context& c, const emu_keyboard_input& input)
        {
            constexpr uint32_t keyeventf_extendedkey = 0x0001;
            constexpr uint32_t keyeventf_keyup = 0x0002;
            constexpr uint32_t keyeventf_unicode = 0x0004;
            constexpr uint32_t keyeventf_scancode = 0x0008;

            const auto window = find_foreground_window(c);
            const bool up = (input.flags & keyeventf_keyup) != 0;
            const bool unicode = (input.flags & keyeventf_unicode) != 0;

            uint32_t virtual_key = input.virtual_key;
            uint32_t scan_code = input.scan_code;
            bool extended = (input.flags & keyeventf_extendedkey) != 0;
            if (unicode)
            {
                virtual_key = VK_PACKET;
            }
            else if (input.flags & keyeventf_scancode)
            {
                const auto extended_scan = (input.flags & keyeventf_extendedkey) ? (0xE000 | scan_code) : scan_code;
                virtual_key = handle_NtUserMapVirtualKeyEx(c, extended_scan, 3, 0, 0);
            }
            else
            {
                const auto extended_scan = handle_NtUserMapVirtualKeyEx(c, virtual_key, 4, 0, 0);
                scan_code = extended_scan & 0xFF;
                extended = extended || (extended_scan >> 8) == 0xE0;
            }

            if (virtual_key == 0 || virtual_key >= c.proc.key_state.size())
            {
                return;
            }

            uint64_t l_param = 1 | (static_cast<uint64_t>(scan_code & 0xFF) << 16);
            l_param |= extended ? (1ull << 24) : 0;
            l_param |= up ? (1ull << 31) : 0;

            // Like Windows, key messages name the generic modifier rather than its left or right variant; the scan code and
            // extended-key bit tell the sides apart.
            const ui_event event{
                .window = window,
                .message = static_cast<uint32_t>(up ? WM_KEYUP : WM_KEYDOWN),
                .wParam = generic_virtual_key(virtual_key),
                .lParam = l_param,
                .injected = true,
                .extra_info = input.extra_info,
                .injected_character = unicode ? static_cast<char16_t>(input.scan_code) : char16_t{},
            };

            // Without a window to receive it, injected input still changes what GetAsyncKeyState reports.
            if (window == 0)
            {
                c.win_emu.update_key_state(event);
                return;
            }

            c.win_emu.handle_ui_event_locked(event);
        }

        uint32_t handle_NtUserSendInput(const syscall_context& c, const uint32_t input_count, const emulator_pointer inputs,
                                        const int32_t input_size)
        {
            constexpr uint32_t input_mouse = 0;
            constexpr uint32_t input_keyboard = 1;
            constexpr uint32_t input_hardware = 2;
            constexpr uint32_t error_invalid_parameter = 87;
            constexpr uint32_t error_noaccess = 998;

            if (input_count == 0 || input_size != sizeof(emu_input) || inputs == 0)
            {
                set_guest_last_error(c, error_invalid_parameter);
                return 0;
            }

            for (uint32_t i = 0; i < input_count; ++i)
            {
                emu_input input{};
                if (!c.win_emu.memory.try_read_memory(inputs + static_cast<uint64_t>(i) * sizeof(emu_input), &input, sizeof(input)))
                {
                    set_guest_last_error(c, error_noaccess);
                    return i;
                }

                if (input.type == input_mouse)
                {
                    send_mouse_input(c, input.mouse);
                }
                else if (input.type == input_keyboard)
                {
                    send_keyboard_input(c, input.keyboard);
                }
                else if (input.type != input_hardware)
                {
                    set_guest_last_error(c, error_invalid_parameter);
                    return i;
                }
            }

            return input_count;
        }

        uint64_t handle_NtUserSetKeyboardState()
        {
            return 0;
        }

        uint64_t handle_NtUserAttachThreadInput()
        {
            return 0;
        }

        BOOL handle_NtUserThreadMessageQueueAttached(const syscall_context& c, const DWORD thread_id)
        {
            const auto thread_info = ensure_win32_thread_info(c);
            publish_win32_thread_info(c, thread_info);
            if (thread_id == 0)
            {
                return FALSE;
            }
            const auto* thread = c.proc.find_thread_by_id(thread_id);
            if (thread == nullptr || thread->is_terminated())
            {
                set_guest_last_error(c, 87);
                return FALSE;
            }
            return thread->win32k_thread_info != 0;
        }

        BOOL handle_NtUserRegisterTouchHitTestingWindow()
        {
            return TRUE;
        }

        BOOL handle_NtUserGetGUIThreadInfo(const syscall_context& c, const uint32_t thread_id, const emulator_pointer info_address)
        {
            if (info_address == 0)
            {
                return FALSE;
            }

            const auto target_thread_id = thread_id == 0 ? c.vcpu.active_thread->id : thread_id;
            const auto* target_thread = c.proc.find_thread_by_id(target_thread_id);
            if (target_thread == nullptr)
            {
                return FALSE;
            }

            const auto* foreground = c.proc.windows.get(c.proc.foreground_window);
            const auto active = foreground && foreground->thread_id == target_thread_id ? c.proc.foreground_window : 0;
            const auto* captured = c.proc.windows.get(c.proc.mouse_capture_window);
            const auto capture = captured && captured->thread_id == target_thread_id ? c.proc.mouse_capture_window : 0;

            struct gui_thread_info
            {
                DWORD cbSize{};
                DWORD flags{};
                hwnd hwndActive{};
                hwnd hwndFocus{};
                hwnd hwndCapture{};
                hwnd hwndMenuOwner{};
                hwnd hwndMoveSize{};
                hwnd hwndCaret{};
                RECT rcCaret{};
            };

            auto info = emulator_object<gui_thread_info>{c.emu, info_address}.read();
            if (info.cbSize != sizeof(info))
            {
                return FALSE;
            }
            info = {.cbSize = sizeof(info), .hwndActive = active, .hwndFocus = target_thread->focus_window, .hwndCapture = capture};
            c.emu.write_memory(info_address, info);

            return TRUE;
        }

        BOOL handle_NtUserSetWinEventHook()
        {
            return TRUE;
        }

        BOOL handle_NtUserUnhookWinEvent()
        {
            return TRUE;
        }

        uint64_t handle_NtUserQueryInputContext(const syscall_context& c, uint64_t, uint32_t)
        {
            set_guest_last_error(c, 6);
            return 0;
        }

        BOOL handle_NtUserDisableThreadIme()
        {
            return TRUE;
        }

        BOOL handle_NtUserGetPointerDevices()
        {
            return FALSE;
        }

        BOOL handle_NtUserHwndQueryRedirectionInfo()
        {
            return FALSE;
        }

        BOOL handle_NtUserEnableNonClientDpiScaling()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetImeHotKey()
        {
            return TRUE;
        }

        int16_t handle_NtUserVkKeyScanEx()
        {
            return -1;
        }

        BOOL handle_NtUserSetWindowFeedbackSetting()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetLayeredWindowAttributes()
        {
            return TRUE;
        }

        BOOL handle_NtUserUpdateLayeredWindow(const syscall_context& c, const hwnd window, const hdc /*destination_dc*/,
                                              const emulator_object<POINT> destination, const emulator_object<SIZE> size,
                                              const hdc source_dc, const emulator_object<POINT> source, const COLORREF /*color_key*/,
                                              const emulator_pointer blend, const DWORD flags, const emulator_pointer /*dirty_rect*/)
        {
            constexpr DWORD ulw_colorkey = 0x1;
            constexpr DWORD ulw_alpha = 0x2;
            constexpr uint8_t ac_src_over = 0x00;
            constexpr uint8_t ac_src_alpha = 0x01;

            auto* win = c.proc.windows.get(window);
            if (!win || (win->ex_style & WS_EX_LAYERED) == 0)
            {
                return FALSE;
            }

            // The colour key would have to be applied while compositing the window, which presentation cannot do.
            // Failing lets the caller fall back instead of showing the keyed colour as opaque.
            if ((flags & ulw_colorkey) != 0)
            {
                return FALSE;
            }

            bool premultiplied_alpha = false;
            auto constant_alpha = uint8_t{255};
            const bool blend_alpha = (flags & ulw_alpha) != 0;
            if (blend_alpha)
            {
                if (blend == 0)
                {
                    return FALSE;
                }

                // BLENDFUNCTION: BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat.
                const auto blend_function = c.emu.read_memory<std::array<uint8_t, 4>>(blend);
                if (blend_function[0] != ac_src_over)
                {
                    return FALSE;
                }

                constant_alpha = blend_function[2];
                premultiplied_alpha = (blend_function[3] & ac_src_alpha) != 0;
            }

            auto x = win->x;
            auto y = win->y;
            auto width = win->width;
            auto height = win->height;
            if (destination)
            {
                const auto position = destination.read();
                x = position.x;
                y = position.y;
            }
            if (size)
            {
                const auto extent = size.read();
                if (extent.cx <= 0 || extent.cy <= 0)
                {
                    return FALSE;
                }
                width = extent.cx;
                height = extent.cy;
            }

            // The layered bitmap covers the whole window rectangle, while the window surface only holds the client area.
            const auto insets = win->nonclient_insets();

            uint32_t source_present_handle{};
            const gdi_bitmap_surface* source_surface = nullptr;
            if (source_dc != 0)
            {
                source_surface = get_dc_present_surface(c, source_dc, source_present_handle);
                if (!source_surface || width - insets.left - insets.right <= 0 || height - insets.top - insets.bottom <= 0)
                {
                    return FALSE;
                }
            }

            win->layered_blend = blend_alpha;
            win->layered_premultiplied_alpha = premultiplied_alpha;
            win->layered_constant_alpha = constant_alpha;

            update_window_geometry(c, *win, x, y, width, height, false);
            if (!source_surface)
            {
                return TRUE;
            }

            const auto origin = source ? source.read() : POINT{};
            const auto surface_width = static_cast<uint32_t>(win->client_width());
            const auto surface_height = static_cast<uint32_t>(win->client_height());
            auto& surface = c.proc.gdi_window_surfaces[static_cast<uint32_t>(win->handle)];
            surface.width = surface_width;
            surface.height = surface_height;
            surface.pixels.assign(static_cast<size_t>(surface_width) * surface_height, 0);

            for (uint32_t row = 0; row < surface_height; ++row)
            {
                const auto source_y = static_cast<int64_t>(origin.y) + insets.top + row;
                if (source_y < 0 || source_y >= source_surface->height)
                {
                    continue;
                }

                for (uint32_t column = 0; column < surface_width; ++column)
                {
                    const auto source_x = static_cast<int64_t>(origin.x) + insets.left + column;
                    if (source_x >= 0 && source_x < source_surface->width)
                    {
                        surface.pixels[(static_cast<size_t>(row) * surface_width) + column] =
                            source_surface->pixels[(static_cast<size_t>(source_y) * source_surface->width) + static_cast<size_t>(source_x)];
                    }
                }
            }

            present_existing_guest_window_surface(c, *win);
            return TRUE;
        }

        DWORD handle_NtUserGetWindowContextHelpId(const syscall_context& c, hwnd window)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return 0;
            }

            return win->context_help_id;
        }

        BOOL handle_NtUserSetWindowContextHelpId(const syscall_context& c, hwnd window, DWORD context_help_id)
        {
            auto* win = c.proc.windows.get(window);
            if (!win)
            {
                set_guest_last_error(c, 1400); // ERROR_INVALID_WINDOW_HANDLE
                return FALSE;
            }

            win->context_help_id = context_help_id;
            return TRUE;
        }

        BOOL handle_NtUserUpdateClientRect(const syscall_context&, const hwnd)
        {
            return TRUE;
        }

        int32_t handle_NtUserScrollWindowEx()
        {
            return 0;
        }

        BOOL handle_NtUserRedrawFrame(const syscall_context&, const hwnd)
        {
            return TRUE;
        }

        BOOL handle_NtUserThunkedMenuInfo(const syscall_context&, const hmenu, const emulator_pointer)
        {
            return TRUE;
        }

        BOOL handle_NtUserShowScrollBar()
        {
            return TRUE;
        }

        BOOL handle_NtUserSetWindowPlacement(const syscall_context&, const hwnd, const emulator_pointer)
        {
            return TRUE;
        }

        BOOL handle_NtUserIsChildWindowDpiMessageEnabled()
        {
            return FALSE;
        }

        ULONG handle_NtUserGetKeyboardType()
        {
            return 0;
        }

        // win32k client-interactivity-tracking telemetry hook; audioses pokes it while starting/stopping a
        // stream. There is nothing to track in the emulator, so acknowledge it.
        NTSTATUS handle_NtUserCitSetInfo()
        {
            return STATUS_SUCCESS;
        }

        // Routine numbers for the Win10 19041-19045 user-call table. Other Windows
        // versions use different numbers, so the table is gated by guest build below.
        constexpr uint32_t user_call_create_menu = 0;
        constexpr uint32_t user_call_create_popup_menu = 1;
        constexpr uint32_t user_call_destroy_caret = 6;
        constexpr uint32_t user_call_disable_process_windows_ghosting = 7;
        constexpr uint32_t user_call_get_message_pos = 12;
        constexpr uint32_t user_call_hide_cursor_no_capture = 16;
        constexpr uint32_t user_call_release_capture = 22;
        constexpr uint32_t user_call_zap_active_and_focus = 25;
        constexpr uint32_t user_call_remote_connect_state = 34;
        constexpr uint32_t user_call_trace_logging_send_mixed_mode_telemetry = 35;
        constexpr uint32_t user_call_begin_defer_window_pos = 44;
        constexpr uint32_t user_call_allow_set_foreground_window = 46;
        constexpr uint32_t user_call_get_keyboard_type = 51;
        constexpr uint32_t user_call_map_desktop_object = 56;
        constexpr uint32_t user_call_message_beep = 57;
        constexpr uint32_t user_call_post_quit_message = 59;
        constexpr uint32_t user_call_swap_mouse_button = 73;
        constexpr uint32_t user_call_thread_message_queue_attached = 78;
        constexpr uint32_t user_call_get_window_context_help_id = 87;
        constexpr uint32_t user_call_set_msg_box = 89;
        constexpr uint32_t user_call_schedule_dispatch_notification = 91;
        constexpr uint32_t user_call_clear_window_state = 95;
        constexpr uint32_t user_call_set_dialog_pointer = 99;
        constexpr uint32_t user_call_set_window_context_help_id = 101;
        constexpr uint32_t user_call_set_window_state = 102;
        constexpr uint32_t user_call_draw_menu_bar = 106;
        constexpr uint32_t user_call_redraw_frame = 109;
        constexpr uint32_t user_call_set_dialog_system_menu = 111;
        constexpr uint32_t user_call_set_foreground_window = 112;
        constexpr uint32_t user_call_set_sys_menu = 113;
        constexpr uint32_t user_call_update_client_rect = 114;
        constexpr uint32_t user_call_update_window = 115;
        constexpr uint32_t user_call_show_owned_popups = 121;
        constexpr uint32_t user_call_enable_window = 125;
        constexpr uint32_t user_call_change_window_message_filter = 126;
        constexpr uint32_t user_call_get_cursor_pos = 127;
        constexpr uint32_t user_call_set_caret_pos = 136;
        constexpr uint32_t user_call_cit_set_info = 140;

        uint64_t handle_NtUserCallHwnd(const syscall_context& c, const hwnd hwnd, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_get_window_context_help_id)
                {
                    return handle_NtUserGetWindowContextHelpId(c, hwnd);
                }

                if (routine == user_call_set_msg_box)
                {
                    return handle_NtUserSetMsgBox(c, hwnd);
                }

                if (routine == user_call_schedule_dispatch_notification)
                {
                    return handle_NtUserScheduleDispatchNotification(c, hwnd);
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwnd routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallHwndLock(const syscall_context& c, const hwnd hwnd, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_draw_menu_bar)
                {
                    return handle_NtUserDrawMenuBar(c, hwnd);
                }

                if (routine == user_call_redraw_frame)
                {
                    return handle_NtUserRedrawFrame(c, hwnd);
                }

                if (routine == user_call_set_dialog_system_menu)
                {
                    return handle_NtUserSetDialogSystemMenu(c, hwnd);
                }

                if (routine == user_call_set_foreground_window)
                {
                    return handle_NtUserSetForegroundWindow(c, hwnd);
                }

                if (routine == user_call_set_sys_menu)
                {
                    return handle_NtUserSetSysMenu(c, hwnd);
                }

                if (routine == user_call_update_client_rect)
                {
                    return handle_NtUserUpdateClientRect(c, hwnd);
                }

                if (routine == user_call_update_window)
                {
                    return handle_NtUserUpdateWindow(c, hwnd);
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwndLock routine: 0x%X\n", routine);
            return FALSE;
        }

        uint64_t handle_NtUserCallHwndParam(const syscall_context& c, const hwnd hwnd, const uint64_t param, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                const auto dword_param = static_cast<uint32_t>(param);

                if (routine == user_call_clear_window_state)
                {
                    return handle_NtUserClearWindowState(c, hwnd, dword_param);
                }

                if (routine == user_call_set_dialog_pointer)
                {
                    return handle_NtUserSetDialogPointer(c, hwnd, param);
                }

                if (routine == user_call_set_window_context_help_id)
                {
                    return handle_NtUserSetWindowContextHelpId(c, hwnd, dword_param);
                }

                if (routine == user_call_set_window_state)
                {
                    return handle_NtUserSetWindowState(c, hwnd, dword_param);
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwndParam routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallHwndParamLock(const syscall_context& c, const hwnd hwnd, const uint64_t param, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                const auto dword_param = static_cast<int32_t>(param);

                if (routine == user_call_show_owned_popups)
                {
                    return handle_NtUserShowOwnedPopups(c, hwnd, dword_param);
                }

                if (routine == user_call_enable_window)
                {
                    return handle_NtUserEnableWindow(c, hwnd, dword_param);
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwndParamLock routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallHwndParamLockSafe(const syscall_context& c, const hwnd hwnd, const uint64_t param, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_enable_window)
                {
                    return handle_NtUserEnableWindow(c, hwnd, static_cast<int32_t>(param));
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwndParamLockSafe routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallHwndSafe(const syscall_context& c, const hwnd hwnd, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_schedule_dispatch_notification)
                {
                    return handle_NtUserScheduleDispatchNotification(c, hwnd);
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallHwndSafe routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallNoParam(const syscall_context& c, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_create_menu)
                {
                    return handle_NtUserCreateMenu(c);
                }

                if (routine == user_call_create_popup_menu)
                {
                    return handle_NtUserCreatePopupMenu(c);
                }

                if (routine == user_call_destroy_caret)
                {
                    return handle_NtUserDestroyCaret();
                }

                if (routine == user_call_disable_process_windows_ghosting)
                {
                    return handle_NtUserDisableProcessWindowsGhosting(c);
                }

                if (routine == user_call_get_message_pos)
                {
                    return handle_NtUserGetMessagePos(c);
                }

                if (routine == user_call_hide_cursor_no_capture)
                {
                    return handle_NtUserHideCursorNoCapture();
                }

                if (routine == user_call_release_capture)
                {
                    return handle_NtUserReleaseCapture(c);
                }

                if (routine == user_call_zap_active_and_focus)
                {
                    return handle_NtUserZapActiveAndFocus(c);
                }

                if (routine == user_call_remote_connect_state)
                {
                    return handle_NtUserRemoteConnectState(c);
                }

                if (routine == user_call_trace_logging_send_mixed_mode_telemetry)
                {
                    return handle_NtUserTraceLoggingSendMixedModeTelemetry();
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallNoParam routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallOneParam(const syscall_context& c, const uint64_t param, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_begin_defer_window_pos)
                {
                    return handle_NtUserBeginDeferWindowPos(c, static_cast<int>(param));
                }

                if (routine == user_call_allow_set_foreground_window)
                {
                    return handle_NtUserAllowSetForegroundWindow();
                }

                if (routine == user_call_get_keyboard_type)
                {
                    return handle_NtUserGetKeyboardType();
                }

                if (routine == user_call_map_desktop_object)
                {
                    const handle handle{.h = param};
                    return handle_NtUserMapDesktopObject(c, handle);
                }

                if (routine == user_call_message_beep)
                {
                    return handle_NtUserMessageBeep();
                }

                if (routine == user_call_post_quit_message)
                {
                    return handle_NtUserPostQuitMessage(c, static_cast<int>(param));
                }

                if (routine == user_call_swap_mouse_button)
                {
                    return handle_NtUserSwapMouseButton();
                }

                if (routine == user_call_thread_message_queue_attached)
                {
                    return handle_NtUserThreadMessageQueueAttached(c, static_cast<int>(param));
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallOneParam routine: 0x%X\n", routine);
            return 0;
        }

        uint64_t handle_NtUserCallTwoParam(const syscall_context& c, const uint64_t param1, const uint64_t param2, const uint32_t routine)
        {
            if (c.win_emu.version.is_build_within(19041, 19046))
            {
                if (routine == user_call_change_window_message_filter)
                {
                    return handle_NtUserChangeWindowMessageFilter();
                }

                if (routine == user_call_get_cursor_pos)
                {
                    // win32kfull!xxxGetCursorPos only accepts the logical (1) and per-monitor DPI (2) cursor modes.
                    if (param2 != 1 && param2 != 2)
                    {
                        return FALSE;
                    }

                    return handle_NtUserGetCursorPos(c, param1);
                }

                if (routine == user_call_set_caret_pos)
                {
                    return handle_NtUserSetCaretPos();
                }

                if (routine == user_call_cit_set_info)
                {
                    return handle_NtUserCitSetInfo();
                }
            }

            c.win_emu.log.error("Unimplemented NtUserCallTwoParam routine: 0x%X\n", routine);
            return 0;
        }
    }

} // namespace sogen
