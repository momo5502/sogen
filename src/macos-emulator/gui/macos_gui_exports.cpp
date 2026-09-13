#include "../std_include.hpp"
#include "macos_gui_exports.hpp"

#include "../macos_platform.hpp"
#include "macos_io_surface_routines.hpp"

#include <array>

namespace sogen
{
    namespace
    {
        constexpr std::string_view skylight = MACOS_SKYLIGHT_IMAGE_PATH;
        constexpr std::string_view core_graphics = MACOS_CORE_GRAPHICS_IMAGE_PATH;
        constexpr std::string_view quartz_core = MACOS_QUARTZ_CORE_IMAGE_PATH;
        constexpr std::string_view hi_services = MACOS_HI_SERVICES_IMAGE_PATH;
        constexpr std::string_view metal = MACOS_METAL_IMAGE_PATH;

        // CGWindowContextCreate is *not* in this table under that name. CoreGraphics exports it as a
        // re-export -- trie flags 0x8 -- so its trie payload is a library ordinal rather than an image
        // offset, and the implementation to intercept is SkyLight's _SLWindowContextCreate. Both
        // _CGWindowContextCreate and _CGWindowContextCreateImage resolve to the same unaligned payload,
        // which is what gave the re-export away.
        //
        // _SLSMainConnectionID, _SLSNewConnection and _SLSGetEventPort are deliberately absent, and so is
        // the SLSRegisterNotifyProc family that used to be intercepted only because those three were.
        // SkyLight's own bring-up builds the CGSConnection object CGSConnectionByID asserts on, and it is
        // the client -- not sogen -- that constructs the event port input is delivered through; sogen
        // answers the bring-up at the MIG layer instead (gui/macos_window_server_mig.cpp).
        constexpr std::array<macos_gui_export, 67> first_pixel{{
            {skylight, "_SLSNewWindow"},
            {skylight, "_SLSReleaseWindow"},
            {skylight, "_SLSSetWindowLevel"},
            {skylight, "_SLSGetWindowLevel"},
            {skylight, "_SLSSetWindowOpacity"},
            {skylight, "_SLSSetWindowAlpha"},
            {skylight, "_SLSSetWindowShape"},
            {skylight, "_SLSSetWindowTags"},
            {skylight, "_SLSSetWindowResolution"},
            {skylight, "_SLSSetWindowTransform"},
            {skylight, "_SLSWindowSetShadowProperties"},
            {skylight, "_SLSGetWindowBounds"},
            {skylight, "_SLSMainDisplayID"},
            {skylight, "_SLSGetDisplayList"},
            {skylight, "_SLSGetOnlineDisplayList"},
            {skylight, "_SLSGetActiveDisplayList"},
            {skylight, "_SLSGetDisplayBounds"},
            {skylight, "_SLSMoveWindow"},
            {skylight, "_SLSOrderWindow"},
            {skylight, "_SLSFlushWindowContentRegion"},
            {skylight, "_SLSDisableUpdate"},
            {skylight, "_SLSReenableUpdate"},
            {skylight, "_SLWindowContextCreate"},
            {core_graphics, "_CGSNewRegionWithRect"},

            // The transaction-based set a 25G76 AppKit/SwiftUI app walks to first frame, per the
            // measured MIG id -> export map. Provenance and semantics:
            {skylight, "_SLSServerPort"},
            {skylight, "_SLSTransactionCreate"},
            {skylight, "_SLSTransactionCommit"},
            {skylight, "_SLSTransactionCommitUsingMethod"},
            {skylight, "_SLSTransactionSetWindowShape"},
            {skylight, "_SLSTransactionOrderWindowGroup"},
            {skylight, "_SLSTransactionOrderWindowGroupFrontConditionally"},
            {skylight, "_SLSNewWindowWithOpaqueShape"},
            {skylight, "_SLSNewWindowWithOpaqueShapeAndContext"},
            {skylight, "_SLSSetWindowLayerContext"},
            {skylight, "_SLSSetWindowTitle"},
            {skylight, "_SLSSetEventMask"},
            {skylight, "_SLSSetWindowClientPerceivedType"},
            {skylight, "_SLSWindowIsOrderedIn"},
            {skylight, "_SLPSRegisterWithServer"},
            {skylight, "_SLPSSetMainApplicationConnection"},
            {skylight, "_SLSSetFrontProcessWithInfo"},
            {skylight, "_SLSGetDockRectWithOrientation"},
            {skylight, "_SLSGetLastUsedKeyboardID"},
            {skylight, "_SLSGetAppearanceThemeLegacy"},
            {skylight, "_SLSCopyDisplayColorSpace"},
            {skylight, "_SLSSetGestureEventSubmask"},
            {skylight, "_SLSCoalesceEventsInMask"},
            {skylight, "_SLPSModifyConnectionNotifications"},
            {skylight, "_SLPSSetNotifications"},
            {skylight, "_SLSPackagesEnableConnectionWindowModificationNotifications"},
            {skylight, "_SLSPackagesEnableConnectionOcclusionNotifications"},
            {quartz_core, "_CARenderServerGetServerPort"},
            {quartz_core, "_CARenderServerGetNeededAlignment"},
            {quartz_core, "_CARenderServerGetMaxRenderableIOSurfaceSize"},

            // The Process Manager surface. AppKit walks it before any window exists, and its
            // registration path aborts rather than degrading when coreservicesd does not answer.
            {hi_services, "_GetCurrentProcess"},
            {hi_services, "__RegisterApplication"},
            {hi_services, "_GetProcessForPID"},
            {hi_services, "_TransformProcessType"},
            {hi_services, "_SetFrontProcess"},
            {hi_services, "_SetFrontProcessWithOptions"},
            {hi_services, "_GetFrontProcess"},
            {hi_services, "_SameProcess"},

            // CF-container answers. CoreGraphics re-exports _CGWindowListCopyWindowInfo and
            // _CGSCopySpacesForWindows from SkyLight, so only SkyLight's names carry an address.
            {skylight, "_SLWindowListCopyWindowInfo"},
            {skylight, "_SLSCopySpacesForWindows"},
            {skylight, "_SLSCopySessionPropertiesTemporaryBridge"},

            // Enumerating Metal devices asks the IOSurface kernel service for the accelerator inventory.
            // sogen answers every other IOSurface call for real -- mach/io_surface_user_client.cpp -- but
            // it emulates no accelerator, and an empty device list is the measured no-GPU path AppKit
            // handles.
            {metal, "_MTLCopyAllDevices"},
            {metal, "_MTLCreateSystemDefaultDevice"},
        }};
    }

    std::span<const macos_gui_export> macos_gui_first_pixel_exports()
    {
        return first_pixel;
    }
}
