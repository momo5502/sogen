#include "std_include.hpp"
#include "keyboard_layout.hpp"

namespace sogen
{
    uint8_t side_virtual_key(const uint32_t virtual_key, const uint64_t key_lparam)
    {
        const bool right_shift = ((key_lparam >> 16) & 0xFF) == 0x36;
        const bool extended = (key_lparam & (1ull << 24)) != 0;
        for (const auto& [generic, left, right] : modifier_sides)
        {
            if (virtual_key == generic)
            {
                const bool is_right = generic == VK_SHIFT ? right_shift : extended;
                return is_right ? right : left;
            }
        }
        return static_cast<uint8_t>(virtual_key);
    }

    // The US layout, which the rest of the keyboard emulation assumes as well.
    char16_t us_layout_character(const uint32_t virtual_key, const bool shift, const bool caps_lock)
    {
        if (virtual_key >= 'A' && virtual_key <= 'Z')
        {
            return static_cast<char16_t>(shift != caps_lock ? virtual_key : virtual_key - 'A' + 'a');
        }

        if (virtual_key >= '0' && virtual_key <= '9')
        {
            constexpr std::u16string_view shifted_digits = u")!@#$%^&*(";
            return shift ? shifted_digits[virtual_key - '0'] : static_cast<char16_t>(virtual_key);
        }

        if (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9)
        {
            return static_cast<char16_t>(u'0' + (virtual_key - VK_NUMPAD0));
        }

        const auto pick = [shift](const char16_t normal, const char16_t shifted) { return shift ? shifted : normal; };

        switch (virtual_key)
        {
        case VK_SPACE:
            return u' ';
        case VK_RETURN:
            return u'\r';
        case VK_BACK:
            return u'\b';
        case VK_TAB:
            return u'\t';
        case VK_ESCAPE:
            return 0x1B;
        case VK_MULTIPLY:
            return u'*';
        case VK_ADD:
            return u'+';
        case VK_SUBTRACT:
            return u'-';
        case VK_DIVIDE:
            return u'/';
        case VK_DECIMAL:
            return u'.';
        case VK_OEM_1:
            return pick(u';', u':');
        case VK_OEM_PLUS:
            return pick(u'=', u'+');
        case VK_OEM_COMMA:
            return pick(u',', u'<');
        case VK_OEM_MINUS:
            return pick(u'-', u'_');
        case VK_OEM_PERIOD:
            return pick(u'.', u'>');
        case VK_OEM_2:
            return pick(u'/', u'?');
        case VK_OEM_3:
            return pick(u'`', u'~');
        case VK_OEM_4:
            return pick(u'[', u'{');
        case VK_OEM_5:
            return pick(u'\\', u'|');
        case VK_OEM_6:
            return pick(u']', u'}');
        case VK_OEM_7:
            return pick(u'\'', u'"');
        default:
            return 0;
        }
    }

    char16_t key_character(const uint32_t virtual_key, const std::array<uint8_t, 256>& keys)
    {
        const auto down = [&keys](const uint32_t key) { return (keys[key] & 0x80) != 0; };

        if (down(VK_CONTROL) && !down(VK_MENU))
        {
            if (virtual_key >= 'A' && virtual_key <= 'Z')
            {
                return static_cast<char16_t>(virtual_key - 'A' + 1);
            }
            if (virtual_key == VK_BACK)
            {
                return 0x7F;
            }
            if (virtual_key == VK_RETURN)
            {
                return u'\n';
            }
            return 0;
        }

        if (down(VK_CONTROL))
        {
            return 0;
        }

        return us_layout_character(virtual_key, down(VK_SHIFT), (keys[VK_CAPITAL] & 0x01) != 0);
    }
}
