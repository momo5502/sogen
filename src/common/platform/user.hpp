#pragma once

#include <cstddef>
#include <cstdint>

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)

#define FNID_START      0x29A
#define FNID_ARRAY_SIZE 24

constexpr size_t USER_NUM_SYSCOLORS = 31;
constexpr size_t USER_SERVERINFO_BRUSH_SLOT_COUNT = 32;
constexpr size_t USER_SERVERINFO_BRUSH_TRAILING_BYTES = 0x78;

struct USER_SERVERINFO
{
    DWORD dwSRVIFlags;
    uint64_t cHandleEntries;
    uint8_t unknown1[0x178];
    uint64_t apfnClientA[FNID_ARRAY_SIZE];
    uint64_t apfnClientW[FNID_ARRAY_SIZE];
    uint64_t apfnClientWorker[FNID_ARRAY_SIZE];
    uint8_t unknown2[0xE90];
    uint64_t ahbrSystem[USER_SERVERINFO_BRUSH_SLOT_COUNT];
    uint8_t unknown3[USER_SERVERINFO_BRUSH_TRAILING_BYTES];
};
static_assert(offsetof(USER_SERVERINFO, apfnClientA) == 0x188);
static_assert(offsetof(USER_SERVERINFO, ahbrSystem) == 0x1258);
static_assert(offsetof(USER_SERVERINFO, unknown3) == 0x1358);
static_assert(sizeof(USER_SERVERINFO) == 0x13D0);

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
    EMULATOR_CAST(uint8_t, USER_HANDLETYPE) bType;
    uint8_t bFlags;
    uint16_t wUniq;
};
static_assert(sizeof(USER_HANDLEENTRY) == 0x20);

struct USER_SHAREDINFO
{
    EMULATOR_CAST(uint64_t, USER_SERVERINFO*) psi;
    EMULATOR_CAST(uint64_t, USER_HANDLEENTRY*) aheList;
    uint32_t HeEntrySize;
    EMULATOR_CAST(uint64_t, USER_DISPINFO*) pDispInfo;
    uint8_t unknown[0xFF];
};

// user32 reads fields after copying 0x238 payload to _gSharedInfo
struct WIN32K_USERCONNECT32
{
    uint32_t psi;
    uint32_t reserved0;
    uint32_t ahe_list;
    uint32_t reserved1;
    uint32_t he_entry_size;
    uint32_t reserved2;
    uint32_t disp_info_low;
    uint32_t reserved3;
    uint8_t reserved4[0x10];
    uint32_t monitor_info_low;
    uint32_t reserved5;
    uint32_t shared_delta_low;
    uint32_t shared_delta_high;
    uint8_t wndmsg_table[0xC8];
    uint32_t wndmsg_count;
    uint32_t reserved6;
    uint32_t wndmsg_bits;
    uint32_t reserved7;
    uint32_t ime_msg_count;
    uint32_t reserved8;
    uint32_t ime_msg_bits;
    uint8_t reserved9[0x114];
};
static_assert(offsetof(WIN32K_USERCONNECT32, ahe_list) == 0x8);
static_assert(offsetof(WIN32K_USERCONNECT32, he_entry_size) == 0x10);
static_assert(offsetof(WIN32K_USERCONNECT32, disp_info_low) == 0x18);
static_assert(offsetof(WIN32K_USERCONNECT32, monitor_info_low) == 0x30);
static_assert(offsetof(WIN32K_USERCONNECT32, wndmsg_count) == 0x108);
static_assert(offsetof(WIN32K_USERCONNECT32, ime_msg_count) == 0x118);
static_assert(sizeof(WIN32K_USERCONNECT32) == 0x238);

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

template <typename Traits>
struct CLSMENUNAME
{
    EMULATOR_CAST(typename Traits::PVOID, char*) pszClientAnsiMenuName;
    EMULATOR_CAST(typename Traits::PVOID, char16_t*) pwszClientUnicodeMenuName;
    EMULATOR_CAST(typename Traits::PVOID, UNICODE_STRING*) pusMenuName;
};

struct USER_CLASS
{
    uint8_t unknown[0xFF];
};

struct USER_WINDOW
{
    uint64_t hWnd;
    uint64_t ptrBase;
    uint8_t pad_010[2];
    uint8_t bFlags;
    uint8_t pad_013[5];
    uint32_t dwExStyle;
    uint32_t dwStyle;
    uint64_t hInstance;
    uint8_t pad_028[8];
    uint64_t spwndParent;
    uint8_t pad_038[64];
    uint64_t lpfnWndProc;
    uint64_t pcls;
    uint8_t pad_088[16];
    uint64_t spmenu;
    uint8_t pad_0A0[40];
    uint32_t cbWndExtra;
    uint8_t pad_0CC[12];
    uint64_t userData;
    uint64_t pActCtx;
    uint8_t pad_0E8[16];
    uint32_t wndExtraOffset;
    uint8_t pad_0FC[44];
    uint64_t pExtraBytes;
    uint8_t pad_130[16];
    uint64_t wID;
};

struct USER_DESKTOPINFO
{
    uint8_t unknown[0xFF];
};

// NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)
