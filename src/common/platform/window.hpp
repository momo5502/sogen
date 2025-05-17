#pragma once

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

using hwnd = pointer;
using hmenu = pointer;
using hinstance = pointer;

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

struct EMU_DISPLAY_DEVICEW
{
    DWORD cb;
    char16_t DeviceName[32];
    char16_t DeviceString[128];
    DWORD StateFlags;
    char16_t DeviceID[128];
    char16_t DeviceKey[128];
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
