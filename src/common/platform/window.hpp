#pragma once

#include "kernel_mapped.hpp"

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

using pointer = uint64_t;

#ifndef OS_WINDOWS
typedef struct tagPOINT
{
    LONG x;
    LONG y;
} POINT;
#endif

using wparam = pointer;
using lparam = pointer;
using lresult = pointer;

typedef struct _LARGE_STRING
{
    ULONG Length;
    ULONG MaximumLength : 31;
    ULONG bAnsi : 1;
    pointer Buffer;
} LARGE_STRING;

using hdc = pointer;
using hwnd = pointer;
using hmenu = pointer;
using hinstance = pointer;
using hicon = pointer;
using hcursor = pointer;
using hbrush = pointer;

struct msg
{
    hwnd window;
    UINT message;
    wparam wParam;
    lparam lParam;
    DWORD time;
    POINT pt;
#ifdef _MAC
    DWORD lPrivate;
#endif
};

struct qmsg
{
    UINT message;
    wparam wParam;
    lparam lParam;
};

struct EMU_WNDCLASSEX
{
    uint32_t cbSize;
    uint32_t style;
    pointer lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    hinstance hInstance;
    hicon hIcon;
    hcursor hCursor;
    hbrush hbrBackground;
    pointer lpszMenuName;
    pointer lpszClassName;
    hicon hIconSm;
};

struct EMU_MINMAXINFO
{
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
};

struct EMU_WINDOWPOS
{
    pointer hwnd;
    pointer hwndInsertAfter;
    int x;
    int y;
    int cx;
    int cy;
    uint32_t flags;
};

struct EMU_CREATESTRUCT
{
    pointer lpCreateParams;
    hinstance hInstance;
    hmenu hMenu;
    hwnd hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    pointer lpszName;
    pointer lpszClass;
    DWORD dwExStyle;
};

#ifndef OS_WINDOWS
#define WS_VISIBLE           0x10000000L

#define SWP_SHOWWINDOW       0x0040
#define SWP_HIDEWINDOW       0x0080

#define WM_CREATE            0x0001
#define WM_DESTROY           0x0002
#define WM_MOVE              0x0003
#define WM_SIZE              0x0005
#define WM_ACTIVATE          0x0006
#define WM_SETFOCUS          0x0007
#define WM_KILLFOCUS         0x0008
#define WM_QUIT              0x0012
#define WM_SHOWWINDOW        0x0018
#define WM_GETMINMAXINFO     0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE          0x0081
#define WM_NCDESTROY         0x0082
#define WM_NCCALCSIZE        0x0083
#define WM_NCACTIVATE        0x0086

#define PM_NOREMOVE          0x0000
#define PM_REMOVE            0x0001
#define PM_NOYIELD           0x0002

#define GWLP_WNDPROC         (-4)
#define GWLP_HINSTANCE       (-6)
#define GWLP_HWNDPARENT      (-8)
#define GWLP_USERDATA        (-21)
#define GWLP_ID              (-12)
#endif

#define WM_UAHDESTROYWINDOW 0x0090

struct EMU_DISPLAY_DEVICEW
{
    DWORD cb;
    char16_t DeviceName[32];
    char16_t DeviceString[128];
    DWORD StateFlags;
    char16_t DeviceID[128];
    char16_t DeviceKey[128];
};

#ifndef ENUM_CURRENT_SETTINGS
#define ENUM_CURRENT_SETTINGS ((DWORD) - 1)
#endif

struct EMU_DEVMODEW
{
    char16_t dmDeviceName[32];
    WORD dmSpecVersion;
    WORD dmDriverVersion;
    WORD dmSize;
    WORD dmDriverExtra;
    DWORD dmFields;
    union
    {
        struct
        {
            int16_t dmOrientation;
            int16_t dmPaperSize;
            int16_t dmPaperLength;
            int16_t dmPaperWidth;
            int16_t dmScale;
            int16_t dmCopies;
            int16_t dmDefaultSource;
            int16_t dmPrintQuality;
        } s;
        POINT dmPosition;
        struct
        {
            POINT dmPosition;
            DWORD dmDisplayOrientation;
            DWORD dmDisplayFixedOutput;
        } s2;
    } u;
    int16_t dmColor;
    int16_t dmDuplex;
    int16_t dmYResolution;
    int16_t dmTTOption;
    int16_t dmCollate;
    char16_t dmFormName[32];
    WORD dmLogPixels;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    union
    {
        DWORD dmDisplayFlags;
        DWORD dmNup;
    } u2;
    DWORD dmDisplayFrequency;
    DWORD dmICMMethod;
    DWORD dmICMIntent;
    DWORD dmMediaType;
    DWORD dmDitherType;
    DWORD dmReserved1;
    DWORD dmReserved2;
    DWORD dmPanningWidth;
    DWORD dmPanningHeight;
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
