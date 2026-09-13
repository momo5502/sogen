#pragma once

#include "../std_include.hpp"
#include "macos_native_dispatch.hpp"

#include <optional>

namespace sogen
{
    class macos_emulator;

    // kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little. On a little-endian machine that is
    // B, G, R, A in memory -- exactly ui_surface_format::bgra8, so nothing converts between the bitmap
    // the guest draws into and the surface the backend is handed. Measured on 25G76 by filling a 1x1
    // context: opaque red reads back `00 00 ff ff` and opaque blue `ff 00 00 ff`. PremultipliedLast
    // (0x2001) is a different layout -- A, B, G, R -- and reading it as BGRA rotates every channel.
    constexpr uint32_t MACOS_CG_BITMAP_INFO_BGRA_PREMULTIPLIED = 0x2002;

    // The one display sogen emulates. A guest may not assume a particular id, the way it may not assume
    // a connection id; this is simply the first one CGDirectDisplayID allows.
    constexpr uint32_t MACOS_MAIN_DISPLAY_ID = 1;

    constexpr int32_t MACOS_CG_ERROR_SUCCESS = 0;
    constexpr int32_t MACOS_CG_ERROR_FAILURE = 1000;
    constexpr int32_t MACOS_CG_ERROR_ILLEGAL_ARGUMENT = 1001;
    constexpr int32_t MACOS_CG_ERROR_INVALID_CONNECTION = 1002;

    struct macos_cg_rect
    {
        double x{};
        double y{};
        double width{};
        double height{};
    };

    std::optional<macos_cg_rect> read_cg_rect(macos_emulator& emu, uint64_t address);
    int32_t clamp_cg_dimension(double value);

    void register_skylight_first_pixel_routines(macos_native_dispatch& dispatch);
}
