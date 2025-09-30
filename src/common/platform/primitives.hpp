#pragma once

#include <cstdint>

// NOLINTBEGIN(modernize-use-using)

#ifdef OS_WINDOWS

#include "../utils/win.hpp"
#include "winnt.h"

#else

#define _Field_size_(...)
#define _Struct_size_bytes_(...)

#define ANYSIZE_ARRAY 1

#define DWORD         std::uint32_t
using LONG = std::int32_t;
using ULONG = DWORD;
using DWORD64 = std::uint64_t;
using ULONG64 = std::uint64_t;
using ULONGLONG = DWORD64;
using LONGLONG = std::int64_t;
using UINT = std::uint32_t;
using BOOL = std::int32_t;

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
#define WCHAR   WORD

#define UCHAR   uint8_t
#define BOOLEAN UCHAR

using CSHORT = int16_t;
using USHORT = WORD;

#define DUMMYSTRUCTNAME

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

static_assert(sizeof(DWORD) == 4);
static_assert(sizeof(ULONG) == 4);
static_assert(sizeof(int) == 4);
static_assert(sizeof(BOOLEAN) == 1);

// NOLINTEND(modernize-use-using)
