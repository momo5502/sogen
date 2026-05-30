#include <windows.h>

#include <string_view>

namespace
{
    constexpr char kWindowClassName[] = "HelloWindowSampleClass";
    constexpr char kWindowTitle[] = "Hello world";

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

int main(int argc, char** argv)
{
    const bool keep_open = argc >= 2 && std::string_view(argv[1]) == "--keep-open";

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &window_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExA(&wc))
    {
        return 1;
    }

    const auto hwnd = CreateWindowExA(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                      640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 2;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!keep_open)
    {
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
    }

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
