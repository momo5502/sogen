#pragma once

#include <array>
#include <cstdint>
#include <platform/platform.hpp>

namespace sogen::gdi
{
    struct charset_info
    {
        uint8_t charset{};
        uint16_t code_page{};

        constexpr uint32_t packed() const
        {
            return (static_cast<uint32_t>(this->charset) << 16) | this->code_page;
        }
    };

    constexpr charset_info resolve_charset(const uint8_t charset, const uint16_t ansi_code_page, const uint16_t oem_code_page)
    {
        constexpr std::array mappings{
            charset_info{ANSI_CHARSET, 1252},   charset_info{EASTEUROPE_CHARSET, 1250}, charset_info{RUSSIAN_CHARSET, 1251},
            charset_info{GREEK_CHARSET, 1253},  charset_info{TURKISH_CHARSET, 1254},    charset_info{HEBREW_CHARSET, 1255},
            charset_info{ARABIC_CHARSET, 1256}, charset_info{BALTIC_CHARSET, 1257},     charset_info{VIETNAMESE_CHARSET, 1258},
            charset_info{THAI_CHARSET, 874},    charset_info{SHIFTJIS_CHARSET, 932},    charset_info{HANGEUL_CHARSET, 949},
            charset_info{JOHAB_CHARSET, 1361},  charset_info{GB2312_CHARSET, 936},      charset_info{CHINESEBIG5_CHARSET, 950},
            charset_info{SYMBOL_CHARSET, 42},   charset_info{OEM_CHARSET, 437},         charset_info{OEM_CHARSET, 850},
        };
        if (charset == ANSI_CHARSET)
        {
            return {ANSI_CHARSET, ansi_code_page};
        }
        if (charset == OEM_CHARSET)
        {
            return {OEM_CHARSET, oem_code_page};
        }
        for (const auto& mapping : mappings)
        {
            if (charset == DEFAULT_CHARSET ? mapping.code_page == ansi_code_page : mapping.charset == charset)
            {
                return mapping;
            }
        }
        return {ANSI_CHARSET, ansi_code_page};
    }
}
