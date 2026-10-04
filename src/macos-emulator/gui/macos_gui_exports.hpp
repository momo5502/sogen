#pragma once

#include <span>
#include <string_view>

namespace sogen
{
    struct macos_gui_export
    {
        std::string_view image{};
        std::string_view symbol{};
    };

    // Provenance: measured on macOS build 25G76, arm64e, by walking each image's export trie out of the
    // shared cache. Regenerate with src/tools/macos-gui-probe/dump-exports.sh.
    constexpr std::string_view MACOS_GUI_EXPORTS_BUILD = "25G76";

    // The window-creation path a program walks when it draws without AppKit. Keyed on the exported client
    // symbol rather than on a MIG routine id: sogen intercepts the caller's side of the boundary, and the
    // ids are the server's, which a client-side patch never reaches.
    std::span<const macos_gui_export> macos_gui_first_pixel_exports();
}
