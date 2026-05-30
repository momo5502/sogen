#include <windows.h>

namespace
{
    constexpr char kWindowClassName[] = "RectWindowSampleClass";

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

    const auto hwnd = CreateWindowExA(0, kWindowClassName, "Rect sample", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 20, 320, 240, nullptr,
                                      nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 2;
    }

    RECT client{};
    RECT window{};
    const auto ok_client = GetClientRect(hwnd, &client);
    const auto ok_window = GetWindowRect(hwnd, &window);

    PostMessageA(hwnd, WM_CLOSE, 0, 0);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (!ok_client || !ok_window)
    {
        return 3;
    }

    if (client.left != 0 || client.top != 0 || client.right != 320 || client.bottom != 240)
    {
        return 4;
    }

    return 0;
}
