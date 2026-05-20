#pragma once

#ifdef OS_WINDOWS
#include <share.h>
#endif

#include <string>
#include <filesystem>
#include <type_traits>

namespace sogen
{

template <typename Traits>
struct UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    EMULATOR_CAST(typename Traits::PVOID, char16_t*) Buffer;
};

template <typename string_type>
    requires(std::is_same_v<string_type, std::u16string> || std::is_same_v<string_type, std::u32string> ||
             std::is_same_v<string_type, std::wstring>)
constexpr string_type convert_from_u8(const std::string_view view)
{
    using char_type = typename string_type::value_type;
    constexpr auto char_size = sizeof(char_type);

    string_type result_str;
    result_str.reserve(view.size());

    bool recheck = false;
    int state = 0;
    uint8_t t1 = 0;
    uint8_t t2 = 0;
    uint8_t t3 = 0;

    for (const char8_t ch : view)
    {
        do
        {
            recheck = false;
            switch (state)
            {
            case 0:
                if (ch <= 0x7f)
                {
                    result_str.push_back(static_cast<char_type>(ch));
                }
                else if (ch >= 0xC2 && ch <= 0xDF)
                {
                    t1 = ch;
                    state = 1;
                }
                else if (ch == 0xE0)
                {
                    t1 = ch;
                    state = 2;
                }
                else if (ch == 0xED)
                {
                    t1 = ch;
                    state = 3;
                }
                else if ((ch >= 0xE1 && ch <= 0xEC) || (ch >= 0xEE && ch <= 0xEF))
                {
                    t1 = ch;
                    state = 4;
                }
                else if (ch == 0xF0)
                {
                    t1 = ch;
                    state = 5;
                }
                else if (ch == 0xF4)
                {
                    t1 = ch;
                    state = 6;
                }
                else if (ch >= 0xF1 && ch <= 0xF3)
                {
                    t1 = ch;
                    state = 7;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                }
                break;
            case 1:
                state = 0;
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    result_str.push_back(static_cast<char_type>(((t1 & 0x1F) << 6) + (ch & 0x3F)));
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    recheck = true;
                }
                break;
            case 2:
                if (ch >= 0xA0 && ch <= 0xBF)
                {
                    t2 = ch;
                    state = 8;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 3:
                if (ch >= 0x80 && ch <= 0x9F)
                {
                    t2 = ch;
                    state = 8;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 4:
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    t2 = ch;
                    state = 8;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 5:
                if (ch >= 0x90 && ch <= 0xBF)
                {
                    t2 = ch;
                    state = 9;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 6:
                if (ch >= 0x80 && ch <= 0x8F)
                {
                    t2 = ch;
                    state = 9;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 7:
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    t2 = ch;
                    state = 9;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 8:
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    result_str.push_back(static_cast<char_type>(((t1 & 0x0F) << 12) + ((t2 & 0x3F) << 6) + (ch & 0x3F)));
                    state = 0;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 9:
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    t3 = ch;
                    state = 10;
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    state = 0;
                    recheck = true;
                }
                break;
            case 10:
                state = 0;
                if (ch >= 0x80 && ch <= 0xBF)
                {
                    uint32_t codepoint = ((t1 & 0x07) << 18) + ((t2 & 0x3F) << 12) + ((t3 & 0x3F) << 6) + (ch & 0x3F);

                    if constexpr (char_size == 2)
                    {
                        if (codepoint <= 0xFFFF)
                        {
                            result_str.push_back(static_cast<char_type>(codepoint));
                        }
                        else
                        {
                            const auto m = codepoint - 0x10000;
                            result_str.push_back(static_cast<char_type>(0xD800 + (m >> 10)));
                            result_str.push_back(static_cast<char_type>(0xDC00 + (m & 0x3ff)));
                        }
                    }
                    else
                    {
                        result_str.push_back(static_cast<char_type>(codepoint));
                    }
                }
                else
                {
                    result_str.push_back(static_cast<char_type>(0xFFFD));
                    recheck = true;
                }
                break;
            default:
                break;
            }
        } while (recheck);
    }

    if (state)
    {
        result_str.push_back(static_cast<char_type>(0xFFFD));
    }

    return result_str;
}

constexpr auto u8_to_u16(const std::string_view view)
{
    return convert_from_u8<std::u16string>(view);
}

constexpr auto u8_to_u32(const std::string_view view)
{
    return convert_from_u8<std::u32string>(view);
}

constexpr auto u8_to_w(const std::string_view view)
{
    return convert_from_u8<std::wstring>(view);
}

template <typename string_view_type>
    requires(std::is_same_v<string_view_type, std::u16string_view> || std::is_same_v<string_view_type, std::u32string_view> ||
             std::is_same_v<string_view_type, std::wstring_view>)
constexpr std::string convert_to_u8(const string_view_type view)
{
    using char_type = typename string_view_type::value_type;
    using uchar_type = std::make_unsigned_t<char_type>;
    constexpr auto char_size = sizeof(char_type);
    constexpr auto upper_codepoint = char_size == 2 ? 0xFFFF : 0x10FFFF;

    std::string utf8_str;
    utf8_str.reserve(view.size() * 2);

    bool recheck = false;
    uint32_t codepoint = 0;
    uint16_t wide = 0;

    // NOLINTNEXTLINE(bugprone-signed-char-misuse)
    for (const uchar_type ch : view)
    {
        do
        {
            recheck = false;
            if (wide)
            {
                if (ch >= 0xDC00 && ch <= 0xDFFF)
                {
                    codepoint = 0x10000 + ((wide - 0xD800) << 10) + (ch - 0xDC00);
                }
                else
                {
                    codepoint = 0xFFFD;
                    recheck = true;
                }
                wide = 0;
            }
            else
            {
                if ((ch <= 0xD7FF || ch >= 0xE000) && ch <= upper_codepoint)
                {
                    codepoint = ch;
                }
                else if (ch >= 0xD800 && ch <= 0xDBFF)
                {
                    if constexpr (char_size == 4)
                    {
                        codepoint = 0xFFFD;
                    }
                    else
                    {
                        wide = ch;
                        continue;
                    }
                }
                else
                {
                    codepoint = 0xFFFD;
                }
            }

            if (codepoint <= 0x7F)
            {
                utf8_str.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint <= 0x7FF)
            {
                utf8_str.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                utf8_str.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint <= 0xFFFF)
            {
                utf8_str.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                utf8_str.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                utf8_str.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint <= 0x10FFFF)
            {
                utf8_str.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                utf8_str.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                utf8_str.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                utf8_str.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        } while (recheck);
    }

    if (wide)
    {
        utf8_str.push_back(static_cast<char>(0xEF));
        utf8_str.push_back(static_cast<char>(0xBF));
        utf8_str.push_back(static_cast<char>(0xBD));
    }

    return utf8_str;
}

constexpr auto u16_to_u8(const std::u16string_view view)
{
    return convert_to_u8<std::u16string_view>(view);
}

constexpr auto u32_to_u8(const std::u32string_view view)
{
    return convert_to_u8<std::u32string_view>(view);
}

constexpr auto w_to_u8(const std::wstring_view view)
{
    return convert_to_u8<std::wstring_view>(view);
}

constexpr auto u16_to_u32(const std::u16string_view view)
{
    std::u32string utf32_str;
    utf32_str.reserve(view.size());

    bool recheck = false;
    uint32_t codepoint = 0;
    uint16_t wide = 0;

    for (const auto ch : view)
    {
        do
        {
            recheck = false;
            if (wide)
            {
                if (ch >= 0xDC00 && ch <= 0xDFFF)
                {
                    codepoint = 0x10000 + ((wide - 0xD800) << 10) + (ch - 0xDC00);
                }
                else
                {
                    codepoint = 0xFFFD;
                    recheck = true;
                }
                wide = 0;
            }
            else
            {
                if (ch <= 0xD7FF || ch >= 0xE000)
                {
                    codepoint = ch;
                }
                else if (ch >= 0xD800 && ch <= 0xDBFF)
                {
                    wide = ch;
                    continue;
                }
                else
                {
                    codepoint = 0xFFFD;
                }
            }

            utf32_str.push_back(codepoint);
        } while (recheck);
    }

    if (wide)
    {
        utf32_str.push_back(0xFFFD);
    }

    return utf32_str;
}

constexpr std::wstring u16_to_w(const std::u16string_view view)
{
    if constexpr (sizeof(wchar_t) == 2)
    {
        return {view.begin(), view.end()};
    }
    else
    {
        const auto u32_str = u16_to_u32(view);
        return {u32_str.begin(), u32_str.end()};
    }
}

#ifndef OS_WINDOWS
inline int open_unicode(FILE** handle, const std::filesystem::path& fileName, const std::u16string& mode)
{
    *handle = fopen(fileName.string().c_str(), u16_to_u8(mode).c_str());
    return errno;
}
#else
inline int open_unicode(FILE** handle, const std::filesystem::path& fileName, const std::u16string& mode)
{
    *handle = _wfsopen(fileName.wstring().c_str(), u16_to_w(mode).c_str(), _SH_DENYNO);
    if (*handle)
    {
        return 0;
    }
    return errno ? errno : EACCES;
}
#endif
} // namespace sogen
