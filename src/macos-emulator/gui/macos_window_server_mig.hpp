#pragma once

#include "../std_include.hpp"
#include "../mach/mach_types.hpp"

#include <cstdint>

namespace sogen
{
    class macos_emulator;
    struct macos_window;
}

namespace sogen::mach
{
    class mig_server_table;
    struct mig_request;
}

namespace sogen
{
    // The WindowServer routine ids SkyLight's own connection bring-up sends, measured on 25G76 and
 // written up The subsystem
    // bases (29000 CGXSession, 30000 CGXServices, 32000 CGXConnection, 34000 CGXDisplay) are the ones
    // in the shared cache's MIG descriptors, so these are wire ids and not relocated slots.
    constexpr int32_t MACOS_MIG_GET_CORE_GRAPHICS_SERVER_VERSION = 29000;
    constexpr int32_t MACOS_MIG_SESSION_DEATH_WATCH_PORT = 29004;
    constexpr int32_t MACOS_MIG_GET_SESSION_PORT = 29010;
    constexpr int32_t MACOS_MIG_GET_WINDOW_SHMEM_REFERENCE = 30082;
    constexpr int32_t MACOS_MIG_SET_PROCESS_NOTIFY_INTERESTS = 30378;
    constexpr int32_t MACOS_MIG_SET_CONNECTION_NOTIFY_INTERESTS = 30379;
    constexpr int32_t MACOS_MIG_GET_DEVICES = 30456;
    constexpr int32_t MACOS_MIG_GET_UNIFIED_KEY_MAPPING = 30553;
    constexpr int32_t MACOS_MIG_NEW_CONNECTION_PORT = 32000;
    constexpr int32_t MACOS_MIG_GET_DEBUG_OPTIONS = 32003;
    constexpr int32_t MACOS_MIG_GET_EVENT_SHMEM = 32006;
    constexpr int32_t MACOS_MIG_GET_DISPLAY_SYSTEM_STATE = 34003;
    constexpr int32_t MACOS_MIG_GET_DISPLAY_SHMEM = 34006;

    // QuartzCore's render-server subsystem, base 40200. Only the registration reaches the wire: the two
    // accessors around it are intercepted at the export level.
    constexpr int32_t MACOS_MIG_CA_REGISTER_CLIENT = 40202;

    // The CoreGraphics client library version SLSServerPort's handshake reads back. 600 is what 25G76's
    // WindowServer answers; SkyLight compares it against its own build to decide whether to keep talking.
    constexpr uint32_t MACOS_CG_SERVER_VERSION = 600;

    // Measured mach_vm_map sizes the guest asks for over the entries 32006 and 34006 hand back. sogen
    // backs each with a whole page, so these are the floor a page has to clear rather than the extent.
    constexpr uint64_t MACOS_EVENT_SCOREBOARD_BYTES = 0x410;
    constexpr uint64_t MACOS_EVENT_SCOREBOARD_AUX_BYTES = 0x3c8;
    constexpr uint64_t MACOS_DISPLAY_SHMEM_HEADER_BYTES = 0x28;
    constexpr uint64_t MACOS_DISPLAY_SHMEM_STATE_BYTES = 0x3180;
    constexpr uint64_t MACOS_DISPLAY_SHMEM_MODES_BYTES = 0x319c;
    constexpr uint64_t MACOS_CONNECTION_SHMEM_BYTES = 0x40;

    // CGSWindowConstructInternal maps the 30082 entry with mach_vm_map(size = 0xb0, cur/max prot READ)
    // and treats a failed map as a reason to return no window at all, which makes AppKit's
    // -[NSCGSWindow initWithConnectionID:flags:] call NSCGSPanic. Layout and the version word are
 //
    constexpr uint64_t MACOS_WINDOW_SHMEM_BYTES = 0xb0;
    constexpr uint32_t MACOS_WINDOW_SHMEM_LAYOUT_VERSION = 0x00030007;

    // _CASRegisterClient checks msgh_size against 0x50 + the 4-byte-aligned length of the server's
    // QuartzCore UUID string and refuses anything else, so the empty-UUID reply is exactly 80 bytes.
    constexpr size_t MACOS_CA_REGISTER_CLIENT_REPLY_BYTES = 0x50;

    // The CGSSessionID every CAHostingToken is minted against. sogen runs one session.
    constexpr uint32_t MACOS_CA_SESSION_ID = 1;

    // The port a bootstrap lookup of com.apple.windowserver.active produces. Minted on first use and
    // shared with SLSServerPort: on the host the root port a client looks up and the session port 29010
    // hands back are the same port, and every 29xxx/30xxx/32xxx/34xxx routine is answered on it.
    mach::port_name_t macos_window_server_session_port(macos_emulator& emu);

    // Rewrites the window's 30082 descriptor page from its current geometry. A window that has never
    // been asked for one has no page and is left alone.
    void macos_window_shmem_refresh(macos_emulator& emu, const macos_window& window);

    void register_window_server_mig_routines(mach::mig_server_table& table);
}
