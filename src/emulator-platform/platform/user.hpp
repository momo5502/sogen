#pragma once

#include <cstddef>
#include <cstdint>

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)

#define FNID_START                0x29A
#define FNID_ARRAY_SIZE           24

#define FNID_SCROLLBAR            0x29A
#define FNID_ICONTITLE            0x29B
#define FNID_MENU                 0x29C
#define FNID_DESKTOP              0x29D
#define FNID_DEFWINDOW            0x29E
#define FNID_MESSAGE              0x29F
#define FNID_SWITCH               0x2A0
#define FNID_BUTTON               0x2A1
#define FNID_COMBOBOX             0x2A2
#define FNID_COMBOLBOX            0x2A3
#define FNID_DIALOG               0x2A4
#define FNID_EDIT                 0x2A5
#define FNID_LISTBOX              0x2A6
#define FNID_MDICLIENT            0x2A7
#define FNID_STATIC               0x2A8
#define FNID_IME                  0x2A9
#define FNID_GHOST                0x2AA
#define FNID_SENDMESSAGE          0x2B1
#define FNID_SENDMESSAGEFF        0x2B2
#define FNID_SENDMESSAGEWTOOPTION 0x2B3
#define FNID_SENDMESSAGECALLBACK  0x2B8

namespace sogen
{

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
        uint8_t unknown3a[0x34];
        int32_t defaultFontHeightScale;
        int32_t defaultFontWidthScale;
        uint8_t unknown3b[0x7C2];
        uint16_t systemDpi;
    };
    static_assert(offsetof(USER_SERVERINFO, apfnClientA) == 0x188);
    static_assert(offsetof(USER_SERVERINFO, ahbrSystem) == 0x1258);
    static_assert(offsetof(USER_SERVERINFO, defaultFontHeightScale) == 0x138C);
    static_assert(offsetof(USER_SERVERINFO, defaultFontWidthScale) == 0x1390);
    static_assert(offsetof(USER_SERVERINFO, systemDpi) == 0x1B56);
    static_assert(sizeof(USER_SERVERINFO) == 0x1B58);

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

    struct USER_WNDMSG
    {
        DWORD maxMsgs;
        uint64_t abMsgs;
    };
    static_assert(offsetof(USER_WNDMSG, abMsgs) == 0x8);
    static_assert(sizeof(USER_WNDMSG) == 0x10);

    struct USER_SHAREDINFO
    {
        EMULATOR_CAST(uint64_t, USER_SERVERINFO*) psi;
        EMULATOR_CAST(uint64_t, USER_HANDLEENTRY*) aheList;
        uint32_t HeEntrySize;
        uint32_t pad_014;
        EMULATOR_CAST(uint64_t, USER_DISPINFO*) pDispInfo;
        uint8_t pad_020[0x78];
        USER_WNDMSG awmControl[FNID_ARRAY_SIZE];
        USER_WNDMSG DefWindowMsgs;
        USER_WNDMSG DefWindowSpecMsgs;
    };
    static_assert(offsetof(USER_SHAREDINFO, pDispInfo) == 0x18);
    static_assert(offsetof(USER_SHAREDINFO, awmControl) == 0x98);
    static_assert(offsetof(USER_SHAREDINFO, DefWindowMsgs) == 0x218);
    static_assert(offsetof(USER_SHAREDINFO, DefWindowSpecMsgs) == 0x228);

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

    // WoW64 (32-bit) raw-input structures, as the guest's user32/win32u marshal them.
    struct RAWINPUTDEVICE32
    {
        uint16_t usUsagePage;
        uint16_t usUsage;
        uint32_t dwFlags;
        uint32_t hwndTarget;
    };
    static_assert(sizeof(RAWINPUTDEVICE32) == 0x0C);

    struct RAWINPUTHEADER32
    {
        uint32_t dwType;
        uint32_t dwSize;
        uint32_t hDevice;
        uint32_t wParam;
    };
    static_assert(sizeof(RAWINPUTHEADER32) == 0x10);

    // The mouse body has no pointer fields, so its layout is identical for 32- and 64-bit guests.
    struct RAWMOUSE32
    {
        uint16_t usFlags;
        uint16_t reserved;
        uint32_t ulButtons;
        uint32_t ulRawButtons;
        int32_t lLastX;
        int32_t lLastY;
        uint32_t ulExtraInformation;
    };
    static_assert(sizeof(RAWMOUSE32) == 0x18);

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
        uint8_t pad_028[2];
        uint16_t fnid;
        uint8_t pad_02C[4];
        uint64_t spwndParent;
        uint64_t spwndChild;
        uint64_t spwndOwner;
        uint64_t spwndNext;
        uint64_t spwndPrev;
        RECT rcWindow;
        RECT rcClient;
        uint64_t lpfnWndProc;
        uint64_t pcls;
        uint8_t pad_088[16];
        uint64_t spmenu;
        uint8_t pad_0A0[24];
        uint32_t dwTextLengthBytes;
        uint8_t pad_0BC[4];
        uint64_t strText;
        uint32_t cbWndExtra;
        uint8_t pad_0CC[12];
        uint64_t userData;
        uint64_t pActCtx;
        uint8_t pad_0E8[4];
        uint32_t windowBand;
        uint8_t pad_0F0[8];
        uint32_t wndExtraOffset;
        uint8_t pad_0FC[44];
        uint64_t pExtraBytes;
        uint8_t pad_130[16];
        uint64_t wID;
    };
    static_assert(sizeof(USER_WINDOW) == 328);

    struct USER_MENU
    {
        uint64_t hMenu;
        uint64_t self;
        uint8_t pad_10[0x10];
        uint64_t rgItems;
        uint32_t flags;
        uint32_t cItems;
    };
    static_assert(offsetof(USER_MENU, hMenu) == 0x0);
    static_assert(offsetof(USER_MENU, self) == 0x8);
    static_assert(offsetof(USER_MENU, rgItems) == 0x20);
    static_assert(offsetof(USER_MENU, flags) == 0x28);
    static_assert(offsetof(USER_MENU, cItems) == 0x2C);

    struct USER_MENU_ITEM
    {
        uint32_t type;
        uint32_t state;
        uint32_t id;
        uint8_t pad_0C[4];
        uint64_t submenu;
        uint64_t hbmpChecked;
        uint64_t hbmpUnchecked;
        uint64_t text;
        uint32_t cch;
        uint8_t pad_34[4];
        uint64_t data;
        uint8_t pad_40[0x20];
        uint64_t hbmpItem;
        uint8_t pad_68[8];
    };
    static_assert(offsetof(USER_MENU_ITEM, state) == 0x4);
    static_assert(offsetof(USER_MENU_ITEM, id) == 0x8);
    static_assert(offsetof(USER_MENU_ITEM, submenu) == 0x10);
    static_assert(offsetof(USER_MENU_ITEM, hbmpChecked) == 0x18);
    static_assert(offsetof(USER_MENU_ITEM, hbmpUnchecked) == 0x20);
    static_assert(offsetof(USER_MENU_ITEM, text) == 0x28);
    static_assert(offsetof(USER_MENU_ITEM, cch) == 0x30);
    static_assert(offsetof(USER_MENU_ITEM, data) == 0x38);
    static_assert(offsetof(USER_MENU_ITEM, hbmpItem) == 0x60);
    static_assert(sizeof(USER_MENU_ITEM) == 0x70);

    struct USER_DESKTOPINFO
    {
        uint8_t unknown0[0x8];
        uint64_t spwndDesktop;
        uint8_t unknown10[0xEF];
    };

    // NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)
} // namespace sogen
