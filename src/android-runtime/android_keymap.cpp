#include "std_include.hpp"
#include "android_keymap.hpp"

#include <platform/window.hpp>

#include <android/keycodes.h>

namespace sogen::android
{
    std::optional<windows_key> map_android_keycode(const int32_t keycode)
    {
        static constexpr uint8_t letter_scan_codes[] = {
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
        };
        static constexpr uint8_t function_scan_codes[] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
        static constexpr uint8_t numpad_scan_codes[] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};

        if (keycode >= AKEYCODE_A && keycode <= AKEYCODE_Z)
        {
            const auto index = keycode - AKEYCODE_A;
            return windows_key{.virtual_key = static_cast<uint8_t>('A' + index), .scan_code = letter_scan_codes[index]};
        }

        if (keycode >= AKEYCODE_0 && keycode <= AKEYCODE_9)
        {
            const auto index = keycode - AKEYCODE_0;
            return windows_key{.virtual_key = static_cast<uint8_t>('0' + index),
                               .scan_code = static_cast<uint8_t>(index == 0 ? 0x0B : 0x01 + index)};
        }

        if (keycode >= AKEYCODE_F1 && keycode <= AKEYCODE_F12)
        {
            const auto index = keycode - AKEYCODE_F1;
            return windows_key{.virtual_key = static_cast<uint8_t>(VK_F1 + index), .scan_code = function_scan_codes[index]};
        }

        if (keycode >= AKEYCODE_NUMPAD_0 && keycode <= AKEYCODE_NUMPAD_9)
        {
            const auto index = keycode - AKEYCODE_NUMPAD_0;
            return windows_key{.virtual_key = static_cast<uint8_t>(VK_NUMPAD0 + index), .scan_code = numpad_scan_codes[index]};
        }

        switch (keycode)
        {
        case AKEYCODE_ESCAPE:
        case AKEYCODE_BACK:
            return windows_key{.virtual_key = VK_ESCAPE, .scan_code = 0x01};
        case AKEYCODE_ENTER:
            return windows_key{.virtual_key = VK_RETURN, .scan_code = 0x1C};
        case AKEYCODE_NUMPAD_ENTER:
            return windows_key{.virtual_key = VK_RETURN, .scan_code = 0x1C, .extended = true};
        case AKEYCODE_DEL:
            return windows_key{.virtual_key = VK_BACK, .scan_code = 0x0E};
        case AKEYCODE_FORWARD_DEL:
            return windows_key{.virtual_key = VK_DELETE, .scan_code = 0x53, .extended = true};
        case AKEYCODE_TAB:
            return windows_key{.virtual_key = VK_TAB, .scan_code = 0x0F};
        case AKEYCODE_SPACE:
            return windows_key{.virtual_key = VK_SPACE, .scan_code = 0x39};
        case AKEYCODE_SHIFT_LEFT:
            return windows_key{.virtual_key = VK_SHIFT, .scan_code = 0x2A};
        case AKEYCODE_SHIFT_RIGHT:
            return windows_key{.virtual_key = VK_SHIFT, .scan_code = 0x36};
        case AKEYCODE_CTRL_LEFT:
            return windows_key{.virtual_key = VK_CONTROL, .scan_code = 0x1D};
        case AKEYCODE_CTRL_RIGHT:
            return windows_key{.virtual_key = VK_CONTROL, .scan_code = 0x1D, .extended = true};
        case AKEYCODE_ALT_LEFT:
            return windows_key{.virtual_key = VK_MENU, .scan_code = 0x38};
        case AKEYCODE_ALT_RIGHT:
            return windows_key{.virtual_key = VK_MENU, .scan_code = 0x38, .extended = true};
        case AKEYCODE_META_LEFT:
            return windows_key{.virtual_key = VK_LWIN, .scan_code = 0x5B, .extended = true};
        case AKEYCODE_META_RIGHT:
            return windows_key{.virtual_key = VK_RWIN, .scan_code = 0x5C, .extended = true};
        case AKEYCODE_MENU:
            return windows_key{.virtual_key = VK_APPS, .scan_code = 0x5D, .extended = true};
        case AKEYCODE_CAPS_LOCK:
            return windows_key{.virtual_key = VK_CAPITAL, .scan_code = 0x3A};
        case AKEYCODE_NUM_LOCK:
            return windows_key{.virtual_key = VK_NUMLOCK, .scan_code = 0x45};
        case AKEYCODE_SCROLL_LOCK:
            return windows_key{.virtual_key = VK_SCROLL, .scan_code = 0x46};
        case AKEYCODE_BREAK:
            return windows_key{.virtual_key = VK_PAUSE, .scan_code = 0x45};
        case AKEYCODE_INSERT:
            return windows_key{.virtual_key = VK_INSERT, .scan_code = 0x52, .extended = true};
        case AKEYCODE_MOVE_HOME:
            return windows_key{.virtual_key = VK_HOME, .scan_code = 0x47, .extended = true};
        case AKEYCODE_MOVE_END:
            return windows_key{.virtual_key = VK_END, .scan_code = 0x4F, .extended = true};
        case AKEYCODE_PAGE_UP:
            return windows_key{.virtual_key = VK_PRIOR, .scan_code = 0x49, .extended = true};
        case AKEYCODE_PAGE_DOWN:
            return windows_key{.virtual_key = VK_NEXT, .scan_code = 0x51, .extended = true};
        case AKEYCODE_DPAD_LEFT:
            return windows_key{.virtual_key = VK_LEFT, .scan_code = 0x4B, .extended = true};
        case AKEYCODE_DPAD_UP:
            return windows_key{.virtual_key = VK_UP, .scan_code = 0x48, .extended = true};
        case AKEYCODE_DPAD_RIGHT:
            return windows_key{.virtual_key = VK_RIGHT, .scan_code = 0x4D, .extended = true};
        case AKEYCODE_DPAD_DOWN:
            return windows_key{.virtual_key = VK_DOWN, .scan_code = 0x50, .extended = true};
        case AKEYCODE_GRAVE:
            return windows_key{.virtual_key = VK_OEM_3, .scan_code = 0x29};
        case AKEYCODE_MINUS:
            return windows_key{.virtual_key = VK_OEM_MINUS, .scan_code = 0x0C};
        case AKEYCODE_EQUALS:
            return windows_key{.virtual_key = VK_OEM_PLUS, .scan_code = 0x0D};
        case AKEYCODE_LEFT_BRACKET:
            return windows_key{.virtual_key = VK_OEM_4, .scan_code = 0x1A};
        case AKEYCODE_RIGHT_BRACKET:
            return windows_key{.virtual_key = VK_OEM_6, .scan_code = 0x1B};
        case AKEYCODE_BACKSLASH:
            return windows_key{.virtual_key = VK_OEM_5, .scan_code = 0x2B};
        case AKEYCODE_SEMICOLON:
            return windows_key{.virtual_key = VK_OEM_1, .scan_code = 0x27};
        case AKEYCODE_APOSTROPHE:
            return windows_key{.virtual_key = VK_OEM_7, .scan_code = 0x28};
        case AKEYCODE_COMMA:
            return windows_key{.virtual_key = VK_OEM_COMMA, .scan_code = 0x33};
        case AKEYCODE_PERIOD:
            return windows_key{.virtual_key = VK_OEM_PERIOD, .scan_code = 0x34};
        case AKEYCODE_SLASH:
            return windows_key{.virtual_key = VK_OEM_2, .scan_code = 0x35};
        case AKEYCODE_NUMPAD_DIVIDE:
            return windows_key{.virtual_key = VK_DIVIDE, .scan_code = 0x35, .extended = true};
        case AKEYCODE_NUMPAD_MULTIPLY:
            return windows_key{.virtual_key = VK_MULTIPLY, .scan_code = 0x37};
        case AKEYCODE_NUMPAD_SUBTRACT:
            return windows_key{.virtual_key = VK_SUBTRACT, .scan_code = 0x4A};
        case AKEYCODE_NUMPAD_ADD:
            return windows_key{.virtual_key = VK_ADD, .scan_code = 0x4E};
        case AKEYCODE_NUMPAD_DOT:
            return windows_key{.virtual_key = VK_DECIMAL, .scan_code = 0x53};
        default:
            return std::nullopt;
        }
    }
}
