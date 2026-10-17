#pragma once

#include <cstdint>
#include <optional>

namespace sogen::android
{
    struct windows_key
    {
        uint8_t virtual_key{};
        uint8_t scan_code{};
        bool extended{};
    };

    std::optional<windows_key> map_android_keycode(int32_t keycode);
}
