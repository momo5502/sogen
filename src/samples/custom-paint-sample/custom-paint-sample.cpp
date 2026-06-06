#include <windows.h>

#include <string>

namespace
{
    constexpr auto* kWindowClassName = "CustomPaintSampleClass";
    constexpr auto* kWindowTitle = "Custom Paint Sample";

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc)
            {
                RECT rect{};
                GetClientRect(hwnd, &rect);

                HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
                FillRect(dc, &rect, bg);
                DeleteObject(bg);

                HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                HGDIOBJ old_pen = SelectObject(dc, pen);
                MoveToEx(dc, 16, 16, nullptr);
                LineTo(dc, rect.right - 16, 16);
                MoveToEx(dc, rect.right - 16, 16, nullptr);
                LineTo(dc, rect.right - 16, rect.bottom - 16);
                MoveToEx(dc, rect.right - 16, rect.bottom - 16, nullptr);
                LineTo(dc, 16, rect.bottom - 16);
                MoveToEx(dc, 16, rect.bottom - 16, nullptr);
                LineTo(dc, 16, 16);

                MoveToEx(dc, 32, 32, nullptr);
                LineTo(dc, 200, 96);
                MoveToEx(dc, 200, 32, nullptr);
                LineTo(dc, 32, 96);

                std::wstring text = L"Hello guest paint ";
                while (text.size() < 700)
                {
                    text += L"x";
                }

                SetBkMode(dc, TRANSPARENT);
                ExtTextOutW(dc, 40, 112, 0, nullptr, text.c_str(), static_cast<UINT>(text.size()), nullptr);
                GdiFlush();

                SelectObject(dc, old_pen);
                DeleteObject(pen);
            }
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

    HWND hwnd = CreateWindowExA(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 320, 180, nullptr, nullptr,
                                wc.hInstance, nullptr);
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
