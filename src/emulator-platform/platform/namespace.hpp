#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "primitives.hpp"

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-use-enum-class)

enum BOUNDARY_ENTRY_TYPE : ULONG
{
    OBNS_Invalid,
    OBNS_Name,
    OBNS_SID,
    OBNS_IL
};

struct OBJECT_BOUNDARY_ENTRY
{
    BOUNDARY_ENTRY_TYPE EntryType;
    ULONG EntrySize;
    // union
    // {
    //     WCHAR Name[1];
    //     PSID Sid;
    //     PSID IntegrityLabel;
    // };
};

struct OBJECT_BOUNDARY_DESCRIPTOR
{
    ULONG Version;
    ULONG Items;
    ULONG TotalSize;
    union
    {
        ULONG Flags;
        struct
        {
            ULONG AddAppContainerSid : 1;
            ULONG Reserved : 31;
        };
    };
    // OBJECT_BOUNDARY_ENTRY Entries[1];
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-use-enum-class)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
