#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

#ifdef OS_WINDOWS
#include <windows.h>

#include <unordered_map>

namespace sogen
{
    namespace
    {
        constexpr wchar_t k_window_class_name[] = L"sogen_ui_host_window";

        std::wstring to_wide(std::u16string_view text)
        {
            return std::wstring(text.begin(), text.end());
        }

        class win32_ui_backend final : public ui_backend
        {
          public:
            void set_event_sink(event_sink sink) override
            {
                this->sink_ = std::move(sink);
            }

            void pump_events() override
            {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }

            void create_window(const ui_window_desc& desc) override
            {
                this->ensure_class();

                const auto parent = this->resolve_hwnd(desc.parent);
                const auto style = this->map_style(desc);
                const auto ex_style = this->map_ex_style(desc);
                const auto title = to_wide(desc.title);
                const auto class_name = this->resolve_class_name(desc.class_name);

                HWND hwnd =
                    CreateWindowExW(ex_style, class_name.c_str(), title.c_str(), style, desc.rect.left, desc.rect.top,
                                    desc.rect.right - desc.rect.left, desc.rect.bottom - desc.rect.top, parent,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(desc.control_id)), GetModuleHandleW(nullptr), this);
                if (!hwnd)
                {
                    printf("HOST create failed class=%s title=%s err=%lu\n", u16_to_u8(desc.class_name).c_str(),
                           u16_to_u8(desc.title).c_str(), GetLastError());
                    return;
                }

                this->host_windows_[desc.handle] = hwnd;
                this->guest_windows_[hwnd] = desc.handle;
                this->update_userdata(hwnd);

                if (desc.owner != 0)
                {
                    if (const auto owner = this->resolve_hwnd(desc.owner))
                    {
                        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
                    }
                }

                ShowWindow(hwnd, desc.visible ? SW_SHOW : SW_HIDE);
                EnableWindow(hwnd, desc.enabled ? TRUE : FALSE);
            }

            void destroy_window(const hwnd window) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    this->guest_windows_.erase(host);
                    this->host_windows_.erase(window);
                    DestroyWindow(host);
                }
            }

            void set_window_rect(const hwnd window, const RECT& rect) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    MoveWindow(host, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
                }
            }

            void set_window_visible(const hwnd window, const bool visible) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    ShowWindow(host, visible ? SW_SHOW : SW_HIDE);
                }
            }

            void set_window_enabled(const hwnd window, const bool enabled) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    EnableWindow(host, enabled ? TRUE : FALSE);
                }
            }

            void set_window_title(const hwnd window, std::u16string_view title) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    const auto wide = to_wide(title);
                    SetWindowTextW(host, wide.c_str());
                }
            }

            void invalidate(const hwnd window, const std::optional<RECT>& rect) override
            {
                if (const auto host = this->resolve_hwnd(window))
                {
                    if (rect)
                    {
                        InvalidateRect(host, &rect.value(), TRUE);
                    }
                    else
                    {
                        InvalidateRect(host, nullptr, TRUE);
                    }
                }
            }

          private:
            static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
            {
                auto* self = reinterpret_cast<win32_ui_backend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (!self)
                {
                    if (msg == WM_NCCREATE)
                    {
                        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
                        self = reinterpret_cast<win32_ui_backend*>(cs->lpCreateParams);
                        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    }
                    else
                    {
                        return DefWindowProcW(hwnd, msg, wparam, lparam);
                    }
                }

                return self->handle_message(hwnd, msg, wparam, lparam);
            }

            void ensure_class()
            {
                static bool registered = false;
                if (registered)
                {
                    return;
                }

                WNDCLASSW wc{};
                wc.lpfnWndProc = &window_proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = k_window_class_name;
                wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
                wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
                RegisterClassW(&wc);
                registered = true;
            }

            std::wstring resolve_class_name(const std::u16string& class_name) const
            {
                if (class_name == u"Button")
                {
                    return L"BUTTON";
                }
                if (class_name == u"Static")
                {
                    return L"STATIC";
                }
                return k_window_class_name;
            }

            HWND resolve_hwnd(const hwnd window) const
            {
                const auto it = this->host_windows_.find(window);
                return it == this->host_windows_.end() ? nullptr : it->second;
            }

            void update_userdata(HWND hwnd)
            {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            }

            DWORD map_style(const ui_window_desc& desc) const
            {
                auto style = static_cast<DWORD>(desc.style);
                if (desc.top_level)
                {
                    if ((style & (WS_CHILD | WS_POPUP | WS_CAPTION)) == 0)
                    {
                        style |= WS_OVERLAPPEDWINDOW;
                    }
                }
                else
                {
                    style |= WS_CHILD;
                }
                return style;
            }

            DWORD map_ex_style(const ui_window_desc& desc) const
            {
                return desc.ex_style;
            }

            LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
            {
                const auto it = this->guest_windows_.find(hwnd);
                const auto guest = it == this->guest_windows_.end() ? 0 : it->second;

                switch (msg)
                {
                case WM_COMMAND:
                    if (this->sink_ && lparam != 0)
                    {
                        const auto child_hwnd = reinterpret_cast<HWND>(lparam);
                        const auto parent_hwnd = GetParent(child_hwnd);
                        const auto child_it = this->guest_windows_.find(child_hwnd);
                        const auto parent_it = this->guest_windows_.find(parent_hwnd);
                        if (child_it != this->guest_windows_.end() && parent_it != this->guest_windows_.end())
                        {
                            this->sink_(ui_event{.window = parent_it->second,
                                                 .message = WM_COMMAND,
                                                 .wParam = static_cast<uint64_t>(wparam),
                                                 .lParam = child_it->second});
                        }
                        else
                        {
                        }
                    }
                    return 0;

                case WM_CLOSE:
                    if (this->sink_ && guest != 0)
                    {
                        this->sink_(ui_event{.window = guest, .message = WM_CLOSE, .wParam = 0, .lParam = 0});
                        return 0;
                    }
                    break;

                case WM_SIZE:
                case WM_MOVE:
                case WM_SHOWWINDOW:
                case WM_SETFOCUS:
                case WM_KILLFOCUS:
                case WM_ACTIVATE:
                case WM_NCACTIVATE:
                case WM_PAINT:
                    break;

                case WM_KEYDOWN:
                case WM_KEYUP:
                case WM_CHAR:
                    if (this->sink_ && guest != 0)
                    {
                        this->sink_(ui_event{.window = guest,
                                             .message = msg,
                                             .wParam = static_cast<uint64_t>(wparam),
                                             .lParam = static_cast<uint64_t>(lparam)});
                        return 0;
                    }
                    break;

                case WM_DESTROY:
                    if (guest != 0)
                    {
                        this->host_windows_.erase(guest);
                        this->guest_windows_.erase(hwnd);
                    }
                    break;
                }

                return DefWindowProcW(hwnd, msg, wparam, lparam);
            }

            event_sink sink_{};
            std::unordered_map<hwnd, HWND> host_windows_{};
            std::unordered_map<HWND, hwnd> guest_windows_{};
        };
    }

    std::unique_ptr<ui_backend> create_default_ui_backend()
    {
        return std::make_unique<win32_ui_backend>();
    }
}
#else
namespace sogen
{
    std::unique_ptr<ui_backend> create_default_ui_backend()
    {
        return std::make_unique<null_ui_backend>();
    }
}
#endif
