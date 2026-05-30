#include <windows.h>

namespace
{
    constexpr char kWindowClassName[] = "PaintWindowSampleClass";

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

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

    const auto hwnd = CreateWindowExA(0, kWindowClassName, "Paint sample", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                      320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 2;
    }

    ShowWindow(hwnd, SW_SHOW);
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
    PostMessageA(hwnd, WM_CLOSE, 0, 0);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
