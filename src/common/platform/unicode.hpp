#pragma once

#include <string>

template <typename Traits>
struct UNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    EMULATOR_CAST(typename Traits::PVOID, char16_t*) Buffer;
};

inline std::string u16_to_u8(const std::u16string_view u16_view)
{
    std::string utf8_str;
    utf8_str.reserve(u16_view.size() * 2);
    for (const char16_t ch : u16_view)
    {
        if (ch <= 0x7F)
        {
            utf8_str.push_back(static_cast<char>(ch));
        }
        else if (ch <= 0x7FF)
        {
            utf8_str.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            utf8_str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
        else
        {
            utf8_str.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            utf8_str.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            utf8_str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return utf8_str;
}

inline std::string w_to_u8(const std::wstring_view w_view)
{
    std::string utf8_str;
    utf8_str.reserve(w_view.size() * 2);
    for (const wchar_t w_ch : w_view)
    {
        const auto ch = static_cast<char16_t>(w_ch);
        if (ch <= 0x7F)
        {
            utf8_str.push_back(static_cast<char>(ch));
        }
        else if (ch <= 0x7FF)
        {
            utf8_str.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            utf8_str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
        else
        {
            utf8_str.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            utf8_str.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            utf8_str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return utf8_str;
}

#ifndef OS_WINDOWS
inline int open_unicode(FILE** handle, const std::u16string& fileName, const std::u16string& mode)
{
    *handle = fopen(u16_to_u8(fileName).c_str(), u16_to_u8(mode).c_str());
    return errno;
}
#else
inline std::wstring u16_to_w(const std::u16string& u16str)
{
    return std::wstring(reinterpret_cast<const wchar_t*>(u16str.data()), u16str.size());
}

inline auto open_unicode(FILE** handle, const std::u16string& fileName, const std::u16string& mode)
{
    return _wfopen_s(handle, u16_to_w(fileName).c_str(), u16_to_w(mode).c_str());
}
#endif
