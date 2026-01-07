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
