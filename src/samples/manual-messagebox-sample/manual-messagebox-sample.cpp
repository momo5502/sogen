#include <windows.h>

#include <cstdio>

namespace
{
    constexpr auto* kWindowClassName = "ManualMessageBoxSampleClass";
    constexpr auto* kWindowTitle = "Question";
    constexpr auto kLabelId = 100;
    constexpr auto kYesId = 101;
    constexpr auto kNoId = 102;
    int g_exit_code = 2;

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE: {
            auto* label = CreateWindowExA(0, "STATIC", "Proceed?", WS_CHILD | WS_VISIBLE, 24, 20, 180, 24, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLabelId)), GetModuleHandleA(nullptr), nullptr);
            auto* yes = CreateWindowExA(0, "BUTTON", "Yes", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 24, 60, 80, 28, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kYesId)), GetModuleHandleA(nullptr), nullptr);
            auto* no = CreateWindowExA(0, "BUTTON", "No", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 120, 60, 80, 28, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNoId)), GetModuleHandleA(nullptr), nullptr);
            if (!label)
            {
                std::printf("label create failed: %lu\n", GetLastError());
            }
            if (!yes)
            {
                std::printf("yes create failed: %lu\n", GetLastError());
            }
            if (!no)
            {
                std::printf("no create failed: %lu\n", GetLastError());
            }
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case kYesId:
                std::puts("clicked: yes");
                g_exit_code = 0;
                DestroyWindow(hwnd);
                return 0;

            case kNoId:
                std::puts("clicked: no");
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

    if (!RegisterClassExA(&wc))
    {
        return 10;
    }

    auto* const hwnd = CreateWindowExA(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 240, 140, nullptr,
                                       nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        return 11;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    for (;;)
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}
