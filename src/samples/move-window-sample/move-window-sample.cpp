#include <windows.h>

namespace
{
    constexpr auto* kWindowClassName = "MoveWindowSampleClass";

    bool check_rect(const RECT& rect, const LONG left, const LONG top, const LONG right, const LONG bottom)
    {
        return rect.left == left && rect.top == top && rect.right == right && rect.bottom == bottom;
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
        }
    }
}

int main()
{
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExA(&wc))
    {
        return 1;
    }

    auto* const hwnd = CreateWindowExA(0, kWindowClassName, "Move window sample", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 20, 320, 240,
                                       nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 2;
    }

    if (!MoveWindow(hwnd, 40, 50, 640, 480, FALSE))
    {
        return 3;
    }

    RECT client{};
    RECT window{};
    if (!GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &window))
    {
        return 4;
    }

    if (!check_rect(client, 0, 0, 640, 480) || !check_rect(window, 40, 50, 680, 530))
    {
        return 5;
    }

    if (!SetWindowPos(hwnd, nullptr, 70, 80, 800, 600, 0))
    {
        return 6;
    }

    if (!GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &window))
    {
        return 7;
    }

    if (!check_rect(client, 0, 0, 800, 600) || !check_rect(window, 70, 80, 870, 680))
    {
        return 8;
    }

    PostMessageA(hwnd, WM_CLOSE, 0, 0);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
