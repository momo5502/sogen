#pragma once

#include <array>
#include <cstdint>

namespace sogen
{
    inline constexpr std::array<std::array<uint8_t, 3>, 3> modifier_sides{{
        {VK_SHIFT, VK_LSHIFT, VK_RSHIFT},
        {VK_CONTROL, VK_LCONTROL, VK_RCONTROL},
        {VK_MENU, VK_LMENU, VK_RMENU},
    }};

    // Key messages name the generic modifier; the scan code (Shift) or the extended-key bit (Ctrl, Alt) in the
    // message's lParam tells which side it is.
    uint8_t side_virtual_key(uint32_t virtual_key, uint64_t key_lparam);

    char16_t us_layout_character(uint32_t virtual_key, bool shift, bool caps_lock);

    // The character a key produces under the given key state, as ToUnicode reports it.
    char16_t key_character(uint32_t virtual_key, const std::array<uint8_t, 256>& keys);
}
