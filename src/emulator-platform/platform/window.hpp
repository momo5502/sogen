#pragma once

#include "kernel_mapped.hpp"

// NOLINTBEGIN(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)

namespace sogen
{

    using pointer = uint64_t;

#ifndef OS_WINDOWS
    typedef DWORD COLORREF;
    typedef ULONG FLONG;

    typedef struct tagPOINT
    {
        LONG x;
        LONG y;
    } POINT;

    typedef struct _POINTL
    {
        LONG x;
        LONG y;
    } POINTL;

    typedef struct tagSIZE
    {
        LONG cx;
        LONG cy;
    } SIZE;
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
    using hdesk = pointer;
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

    struct EMU_PAINTSTRUCT
    {
        hdc paint_hdc;
        BOOL fErase;
        RECT rcPaint;
        BOOL fRestore;
        BOOL fIncUpdate;
        uint8_t rgbReserved[32];
    };

#define EMU_HWND_MESSAGE ((hwnd) - 3)

#ifndef OS_WINDOWS
#define MAXINTATOM           0xC000

#define WS_POPUP             0x80000000L
#define WS_CHILD             0x40000000L
#define WS_VISIBLE           0x10000000L
#define WS_DISABLED          0x08000000L
#define WS_CLIPSIBLINGS      0x04000000L
#define WS_CLIPCHILDREN      0x02000000L

#define SWP_NOSIZE           0x0001
#define SWP_NOMOVE           0x0002
#define SWP_NOREDRAW         0x0008
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
#define WM_SETTEXT           0x000C
#define WM_PAINT             0x000F
#define WM_CLOSE             0x0010
#define WM_ERASEBKGND        0x0014
#define WM_GETMINMAXINFO     0x0024
#define WM_KEYDOWN           0x0100
#define WM_KEYUP             0x0101
#define WM_CHAR              0x0102
#define WM_MOUSEMOVE         0x0200
#define WM_LBUTTONDOWN       0x0201
#define WM_LBUTTONUP         0x0202
#define WM_RBUTTONDOWN       0x0204
#define WM_RBUTTONUP         0x0205
#define WM_COMMAND           0x0111
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED  0x0047
#define WM_NCCREATE          0x0081
#define WM_NCDESTROY         0x0082
#define WM_NCCALCSIZE        0x0083
#define WM_NCACTIVATE        0x0086

#define BN_CLICKED           0

#define MK_LBUTTON           0x0001

#define VK_ESCAPE            0x1B
#define VK_RETURN            0x0D

#define IDOK                 1
#define IDCANCEL             2
#define IDABORT              3
#define IDRETRY              4
#define IDIGNORE             5
#define IDYES                6
#define IDNO                 7

#define PM_NOREMOVE          0x0000
#define PM_REMOVE            0x0001
#define PM_NOYIELD           0x0002

#define RDW_INVALIDATE       0x0001
#define RDW_INTERNALPAINT    0x0002
#define RDW_ERASE            0x0004
#define RDW_VALIDATE         0x0008
#define RDW_NOINTERNALPAINT  0x0010
#define RDW_NOERASE          0x0020
#define RDW_UPDATENOW        0x0100
#define RDW_ERASENOW         0x0200

#define ETO_OPAQUE           0x0002
#define ETO_CLIPPED          0x0004

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

#ifndef OS_WINDOWS
    struct LUID
    {
        DWORD LowPart;
        LONG HighPart;
    };

    struct POINTL
    {
        LONG x;
        LONG y;
    };

    struct RECTL
    {
        LONG left;
        LONG top;
        LONG right;
        LONG bottom;
    };

    struct DISPLAYCONFIG_PATH_SOURCE_INFO
    {
        LUID adapterId;
        UINT32 id;
        union
        {
            UINT32 modeInfoIdx;
            struct
            {
                UINT32 cloneGroupId : 16;
                UINT32 sourceModeInfoIdx : 16;
            } s;
        } u;
        UINT32 statusFlags;
    };

    struct DISPLAYCONFIG_RATIONAL
    {
        UINT32 Numerator;
        UINT32 Denominator;
    };

    struct DISPLAYCONFIG_2DREGION
    {
        UINT32 cx;
        UINT32 cy;
    };

    struct DISPLAYCONFIG_DESKTOP_IMAGE_INFO
    {
        POINTL PathSourceSize;
        RECTL DesktopImageRegion;
        RECTL DesktopImageClip;
    };
#endif

    struct EMU_DISPLAYCONFIG_PATH_TARGET_INFO
    {
        LUID adapterId;
        UINT32 id;
        union
        {
            UINT32 modeInfoIdx;
            struct
            {
                UINT32 desktopModeInfoIdx : 16;
                UINT32 targetModeInfoIdx : 16;
            } s;
        } u;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY) outputTechnology;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_ROTATION) rotation;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_SCALING) scaling;
        DISPLAYCONFIG_RATIONAL refreshRate;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_SCANLINE_ORDERING) scanLineOrdering;
        BOOL targetAvailable;
        UINT32 statusFlags;
    };

    struct EMU_DISPLAYCONFIG_PATH_INFO
    {
        DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo;
        EMU_DISPLAYCONFIG_PATH_TARGET_INFO targetInfo;
        UINT32 flags;
    };

    struct EMU_DISPLAYCONFIG_VIDEO_SIGNAL_INFO
    {
        UINT64 pixelRate;
        DISPLAYCONFIG_RATIONAL hSyncFreq;
        DISPLAYCONFIG_RATIONAL vSyncFreq;
        DISPLAYCONFIG_2DREGION activeSize;
        DISPLAYCONFIG_2DREGION totalSize;
        union
        {
            struct
            {
                UINT32 videoStandard : 16;
                UINT32 vSyncFreqDivider : 6;
                UINT32 reserved : 10;
            } AdditionalSignalInfo;
            UINT32 videoStandard;
        } u;
        uint32_t scanLineOrdering;
    };

    struct EMU_DISPLAYCONFIG_TARGET_MODE
    {
        EMU_DISPLAYCONFIG_VIDEO_SIGNAL_INFO targetVideoSignalInfo;
    };

    struct EMU_DISPLAYCONFIG_SOURCE_MODE
    {
        UINT32 width;
        UINT32 height;
        uint32_t pixelFormat;
        POINTL position;
    };

    struct EMU_DISPLAYCONFIG_MODE_INFO
    {
        uint32_t infoType;
        UINT32 id;
        LUID adapterId;
        union
        {
            EMU_DISPLAYCONFIG_TARGET_MODE targetMode;
            EMU_DISPLAYCONFIG_SOURCE_MODE sourceMode;
            DISPLAYCONFIG_DESKTOP_IMAGE_INFO desktopImageInfo;
        } u;
    };

    enum class EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE : UINT32
    {
        GET_SOURCE_NAME = 1,
        GET_TARGET_NAME = 2,
        GET_TARGET_PREFERRED_MODE = 3,
        GET_ADAPTER_NAME = 4,
        SET_TARGET_PERSISTENCE = 5,
        GET_TARGET_BASE_TYPE = 6,
        GET_SUPPORT_VIRTUAL_RESOLUTION = 7,
        SET_SUPPORT_VIRTUAL_RESOLUTION = 8,
        GET_ADVANCED_COLOR_INFO = 9,
        SET_ADVANCED_COLOR_STATE = 10,
        GET_SDR_WHITE_LEVEL = 11,
        GET_MONITOR_SPECIALIZATION = 12,
        SET_MONITOR_SPECIALIZATION = 13,
        SET_RESERVED1 = 14,
        GET_ADVANCED_COLOR_INFO_2 = 15,
        SET_HDR_STATE = 16,
        SET_WCG_STATE = 17,

        GET_DISPLAY_INFO = static_cast<uint32_t>(-2),
        GET_SOURCE_FROM_HASH = static_cast<uint32_t>(-14),
        GET_DISPLAY_INFO_EX = static_cast<uint32_t>(-21),
    };

    struct EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER
    {
        EMU_DISPLAYCONFIG_DEVICE_INFO_TYPE type;
        UINT32 size;
        LUID adapterId;
        UINT32 id;
    };

    struct EMU_DISPLAYCONFIG_SOURCE_DEVICE_NAME
    {
        EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        char16_t viewGdiDeviceName[32];
    };

    struct EMU_DISPLAYCONFIG_TARGET_DEVICE_NAME
    {
        EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_TARGET_DEVICE_NAME_FLAGS) flags;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY) outputTechnology;
        UINT16 edidManufactureId;
        UINT16 edidProductCodeId;
        UINT32 connectorInstance;
        char16_t monitorFriendlyDeviceName[64];
        char16_t monitorDevicePath[128];
    };

    struct EMU_DISPLAYCONFIG_ADAPTER_NAME
    {
        EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        char16_t adapterDevicePath[128];
    };

    struct EMU_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO
    {
        EMU_DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        union
        {
            struct
            {
                UINT32 advancedColorSupported : 1;     // A type of advanced color is supported
                UINT32 advancedColorEnabled : 1;       // A type of advanced color is enabled
                UINT32 wideColorEnforced : 1;          // Wide color gamut is enabled
                UINT32 advancedColorForceDisabled : 1; // Advanced color is force disabled due to system/OS policy
                UINT32 reserved : 28;
            } s;
            UINT32 value;
        } u;
        EMULATOR_CAST(uint32_t, DISPLAYCONFIG_COLOR_ENCODING) colorEncoding;
        UINT32 bitsPerColorChannel;
    };

    // NOLINTEND(modernize-use-using,cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-use-enum-class)
} // namespace sogen
