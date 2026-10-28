#pragma once

#include <array>
#include <cstdint>

namespace sogen
{
    char16_t us_layout_character(uint32_t virtual_key, bool shift, bool caps_lock);

    // The character a key produces under the given key state, as ToUnicode reports it.
    char16_t key_character(uint32_t virtual_key, const std::array<uint8_t, 256>& keys);
}
