#pragma once

#include <array>
#include <cstdint>

#include <platform/platform.hpp>

namespace sogen
{
    struct gdi_font_signature
    {
        std::array<uint32_t, 4> unicode_subsets{};
        std::array<uint32_t, 2> code_pages{};
    };

    constexpr gdi_font_signature make_gdi_font_signature(const uint8_t charset)
    {
        gdi_font_signature signature{};

        const auto set_unicode_subset = [&](const uint32_t bit) { signature.unicode_subsets[bit / 32] |= uint32_t{1} << (bit % 32); };
        const auto set_code_page = [&](const uint32_t bit) { signature.code_pages[bit / 32] |= uint32_t{1} << (bit % 32); };

        set_unicode_subset(0);

        switch (charset)
        {
        case ANSI_CHARSET:
            set_unicode_subset(1);
            set_code_page(0);
            break;
        case EASTEUROPE_CHARSET:
            set_unicode_subset(1);
            set_unicode_subset(2);
            set_code_page(1);
            break;
        case RUSSIAN_CHARSET:
            set_unicode_subset(9);
            set_code_page(2);
            break;
        case GREEK_CHARSET:
            set_unicode_subset(7);
            set_code_page(3);
            break;
        case TURKISH_CHARSET:
            set_unicode_subset(1);
            set_code_page(4);
            break;
        case HEBREW_CHARSET:
            set_unicode_subset(11);
            set_code_page(5);
            break;
        case ARABIC_CHARSET:
            set_unicode_subset(13);
            set_code_page(6);
            break;
        case BALTIC_CHARSET:
            set_unicode_subset(1);
            set_unicode_subset(2);
            set_code_page(7);
            break;
        case VIETNAMESE_CHARSET:
            set_unicode_subset(1);
            set_unicode_subset(29);
            set_code_page(8);
            break;
        case SHIFTJIS_CHARSET:
            set_unicode_subset(49);
            set_unicode_subset(50);
            set_unicode_subset(59);
            set_code_page(17);
            break;
        default:
            break;
        }

        return signature;
    }
}
