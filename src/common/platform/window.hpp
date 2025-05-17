#pragma once

using pointer = uint64_t;

#ifndef OS_WINDOWS
typedef struct tagPOINT
{
    LONG x;
    LONG y;
} POINT, *PPOINT, NEAR *NPPOINT, FAR *LPPOINT;
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
    hwnd hwnd;
    UINT message;
    wparam wParam;
    lparam lParam;
    DWORD time;
    POINT pt;
#ifdef _MAC
    DWORD lPrivate;
#endif
};
