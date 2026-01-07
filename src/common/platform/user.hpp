#pragma once

#include <cstdint>

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)

struct USER_SERVERINFO
{
    DWORD dwSRVIFlags;
    uint64_t cHandleEntries;
    uint8_t unknown[0x1000];
};

struct USER_DISPINFO
{
    DWORD dwMonitorCount;
    EMULATOR_CAST(uint64_t, USER_MONITOR*) pPrimaryMonitor;
    uint8_t unknown[0xFF];
};

struct USER_HANDLEENTRY
{
    uint64_t pHead;
    uint64_t pOwner;
    uint64_t unknown;
    uint8_t bType;
    uint8_t bFlags;
    uint16_t wUniq;
};
static_assert(sizeof(USER_HANDLEENTRY) == 0x20);

struct USER_SHAREDINFO
{
    uint64_t psi;
    uint64_t aheList;
    uint32_t HeEntrySize;
    uint64_t pDispInfo;
    uint8_t unknown[0xFF];
};

struct USER_THROBJHEAD
{
    struct
    {
        uint64_t h;
        uint32_t cLockObj;
    } h;
    uint64_t pti;
};

struct USER_THRDESKHEAD
{
    USER_THROBJHEAD h;
    uint64_t rpdesk;
    uint64_t pSelf;
};

enum USER_HANDLETYPE : uint8_t
{
    TYPE_FREE = 0,
    TYPE_WINDOW = 1,
    TYPE_MENU = 2,
    TYPE_CURSOR = 3,
    TYPE_SETWINDOWPOS = 4,
    TYPE_HOOK = 5,
    TYPE_CLIPDATA = 6,
    TYPE_CALLPROC = 7,
    TYPE_ACCELTABLE = 8,
    TYPE_DDEACCESS = 9,
    TYPE_DDECONV = 10,
    TYPE_DDEXACT = 11,
    TYPE_MONITOR = 12,
    TYPE_KBDLAYOUT = 13,
    TYPE_KBDFILE = 14,
    TYPE_WINEVENTHOOK = 15,
    TYPE_TIMER = 16,
    TYPE_INPUTCONTEXT = 17,
    TYPE_HIDDATA = 18,
    TYPE_DEVICEINFO = 19,
    TYPE_TOUCHINPUTINFO = 20,
    TYPE_GESTUREINFOOBJ = 21,
    TYPE_CTYPES = 22,
    TYPE_GENERIC = 255
};

struct USER_MONITOR
{
    EMULATOR_CAST(uint64_t, HMONITOR) hmon;
    uint8_t unknown1[0x14];
    RECT rcMonitor;
    RECT rcWork;
    union
    {
        struct
        {
            uint16_t monitorDpi;
            uint16_t nativeDpi;
        } b26;
        struct
        {
            uint32_t unknown1;
            uint16_t monitorDpi;
            uint16_t nativeDpi;
            uint16_t cachedDpi;
            uint16_t unknown2;
            RECT rcMonitorDpiAware;
        } b20;
    };
    uint8_t unknown4[0xFF];
};

struct USER_WINDOW
{
    uint8_t unknown[0xFF];
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
