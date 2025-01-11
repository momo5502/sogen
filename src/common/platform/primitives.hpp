#pragma once

#include <cstdint>

#ifdef OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "winnt.h"

#else

#define _Field_size_(...)
#define _Struct_size_bytes_(...)

#define ANYSIZE_ARRAY 1

#define DWORD         std::uint32_t
using LONG = std::int32_t;
using ULONG = DWORD;
using DWORD64 = std::uint64_t;
using ULONGLONG = DWORD64;
using LONGLONG = std::int64_t;

typedef union _ULARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    };

    ULONGLONG QuadPart;
} ULARGE_INTEGER;

typedef union _LARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        LONG HighPart;
    };

    LONGLONG QuadPart;
} LARGE_INTEGER;

using BYTE = std::uint8_t;
#define CHAR          BYTE
#endif

using WORD = std::uint16_t;

#define UCHAR   unsigned char
#define BOOLEAN UCHAR

using CSHORT = short;
using USHORT = WORD;

#define DUMMYSTRUCTNAME

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
