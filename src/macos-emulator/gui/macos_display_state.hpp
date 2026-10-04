#pragma once

#include "../std_include.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace sogen
{
    // The generation sogen's display configuration carries. 34003's request holds the generation the
    // client already has and its reply is everything newer, so a constant means the blob is serialized
    // once and every later request is answered as empty -- which is what keeps the client's cached
    // CGSDisplaySystemState alive rather than replacing it on every query.
    constexpr uint64_t MACOS_DISPLAY_STATE_GENERATION = 1;

    // Points per inch the emulated panel is described with. It only sets the physical extent and the
    // per-mode DPI the client reports back; nothing in sogen scales by it.
    constexpr double MACOS_DISPLAY_DPI = 96.0;

    struct macos_display_description
    {
        uint32_t id{};
        double x{};
        double y{};
        double width{};
        double height{};
        double scale{1.0};
        double refresh_hz{60.0};

        // SLSGetDisplayMenubarHeight reads this straight out of the record. Zero means sogen states no
        // height of its own and AppKit falls back to its built-in one (measured: 24 points).
        uint32_t menubar_height{};
    };

    // The UUID SLSCopyDisplayUUID hands back for a display, and the key -[NSScreen _screenForUUIDString:]
    // matches on. RFC 4122 version 4 shaped so CFUUIDCreateString produces a well-formed string.
    std::array<uint8_t, 16> macos_display_uuid(uint32_t display_id);

    // The out-of-line payload of MIG 34003 GetDisplaySystemState: the wire form of the whole
    // CGSDisplaySystemState the SkyLight client decodes with CFDataGetBytes and then answers every
    // display, mode and UUID query out of. Layout measured on 25G76 and written up in
    std::vector<uint8_t> macos_build_display_system_state(std::span<const macos_display_description> displays, uint64_t generation);
}
