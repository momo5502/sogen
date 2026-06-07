#include <windows.h>

#include <array>
#include <cstdio>

// A slightly richer manual UI sample (no dialog template) used to exercise the emulated
// USER/GDI paint path with several builtin control variations: a group box, an auto-checkbox,
// an auto-radio-button group, multiple static labels (one updated at runtime), and default vs.
// normal push buttons. All controls are created by hand in WM_CREATE so the create/paint/click
// flow stays easy to trace.

namespace
{
    constexpr auto* kWindowClassName = "ManualControlsSampleClass";
    constexpr auto* kWindowTitle = "Controls";

    enum class control_id : int
    {
        title = 100,
        group = 101,
        check = 102,
        mode_label = 103,
        radio_fast = 104,
        radio_accurate = 105,
        status = 106,
        ok = 107,
        cancel = 108,
    };

    constexpr int id(control_id value)
    {
        return static_cast<int>(value);
    }

    int g_exit_code = 2;
    HWND g_check = nullptr;
    HWND g_radio_fast = nullptr;
    HWND g_radio_accurate = nullptr;
    HWND g_status = nullptr;

    HWND create_control(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id)
    {
        auto* const control = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleA(nullptr), nullptr);
        if (!control)
        {
            std::printf("create '%s' (id=%d) failed: %lu\n", text, id, GetLastError());
        }

        return control;
    }

    void update_status(HWND hwnd)
    {
        const bool checked = SendMessageA(g_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool fast = SendMessageA(g_radio_fast, BM_GETCHECK, 0, 0) == BST_CHECKED;

        std::array<char, 128> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "Status: feature %s, mode %s", checked ? "on" : "off", fast ? "fast" : "accurate");
        SetWindowTextA(g_status, buffer.data());
        (void)hwnd;
    }

    void on_create(HWND hwnd)
    {
        create_control(hwnd, "STATIC", "Configure the sample:", 0, 16, 12, 320, 20, id(control_id::title));

        create_control(hwnd, "BUTTON", "Options", BS_GROUPBOX, 16, 40, 320, 116, id(control_id::group));
        g_check = create_control(hwnd, "BUTTON", "Enable feature", BS_AUTOCHECKBOX, 32, 64, 200, 22, id(control_id::check));

        create_control(hwnd, "STATIC", "Mode:", 0, 32, 96, 48, 20, id(control_id::mode_label));
        g_radio_fast = create_control(hwnd, "BUTTON", "Fast", BS_AUTORADIOBUTTON | WS_GROUP, 88, 94, 90, 22, id(control_id::radio_fast));
        g_radio_accurate = create_control(hwnd, "BUTTON", "Accurate", BS_AUTORADIOBUTTON, 184, 94, 120, 22, id(control_id::radio_accurate));

        g_status = create_control(hwnd, "STATIC", "Status: feature off, mode fast", WS_GROUP, 16, 168, 320, 20, id(control_id::status));

        create_control(hwnd, "BUTTON", "OK", BS_DEFPUSHBUTTON, 160, 208, 80, 28, id(control_id::ok));
        create_control(hwnd, "BUTTON", "Cancel", BS_PUSHBUTTON, 252, 208, 80, 28, id(control_id::cancel));

        // Default the radio group to "Fast".
        SendMessageA(g_radio_fast, BM_SETCHECK, BST_CHECKED, 0);
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
            on_create(hwnd);
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case id(control_id::check):
            case id(control_id::radio_fast):
            case id(control_id::radio_accurate):
                update_status(hwnd);
                return 0;

            case id(control_id::ok): {
                const bool checked = SendMessageA(g_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool fast = SendMessageA(g_radio_fast, BM_GETCHECK, 0, 0) == BST_CHECKED;
                std::printf("clicked: ok (feature=%s, mode=%s)\n", checked ? "on" : "off", fast ? "fast" : "accurate");
                g_exit_code = 0;
                DestroyWindow(hwnd);
                return 0;
            }

            case id(control_id::cancel):
                std::puts("clicked: cancel");
                g_exit_code = 1;
                DestroyWindow(hwnd);
                return 0;

            default:
                break;
            }
            break;

        case WM_CLOSE:
            std::puts("wm_close");
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            std::puts("wm_destroy");
            PostQuitMessage(g_exit_code);
            return 0;

        default:
            break;
        }

        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

int main()
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    if (!RegisterClassExA(&wc))
    {
        return 10;
    }

    auto* const hwnd = CreateWindowExA(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 368, 296, nullptr,
                                       nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 11;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}
