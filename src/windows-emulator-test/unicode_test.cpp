#include <gtest/gtest.h>

#include "common/platform/platform.hpp"
#include <cstring>

namespace test
{
    const std::vector<std::pair<const char*, const char16_t*>> good_strings = {
        {"", u""},
        {"\x48\x65\x6c\x6c\x6f\x20\x7c\x20\x48\xc3\xa4\x6a\x20\xc4\x85\xcc\x8a\x20\x64\x69\x67\x20"
         "\x7c\x20\xce\x93\xce\xb5\xce\xb9\xce\xac\x20\xcf\x83\xce\xb1\xcf\x82\x20\x7c\x20\xd0\x9f"
         "\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\x7c\x20\xe1\x83\x92\xe1\x83\x90\xe1\x83\x9b"
         "\xe1\x83\x90\xe1\x83\xa0\xe1\x83\xaf\xe1\x83\x9d\xe1\x83\x91\xe1\x83\x90\x20\x7c\x20\xe0"
         "\xae\xb5\xe0\xae\xa3\xe0\xae\x95\xe0\xaf\x8d\xe0\xae\x95\xe0\xae\xae\xe0\xaf\x8d\x20\x7c"
         "\x20\xe4\xbb\x8a\xe6\x97\xa5\xe6\x8b\x9d\xe3\x81\xaa\xe3\x81\xb3\xe3\x82\x89\x20\x7c\x20"
         "\xe0\xa4\xb6\xe0\xa5\x81\xe0\xa4\xad\x20\xe0\xa4\xaa\xe0\xa5\x8d\xe0\xa4\xaf\xe0\xa4\xbe"
         "\xe0\xa4\xb0\x20\x7c\x20\xe1\x83\x92\xe1\x83\x94\xe1\x83\x92\xe1\x83\x90\xe1\x83\xaf\xe1"
         "\x83\x92\xe1\x83\x98\xe1\x83\x9c\xe1\x83\x90\xe1\x83\xa1\x20\x7c\x20\xec\x95\x88\xeb\x85"
         "\x95\xed\x95\x98\xec\x8b\xad\xeb\x8b\x88\xea\xb9\x8c\x20\x7c\x20\xe5\x93\x88\xe5\x9b\x89"
         "\x20\x7c\x20\xe0\xaa\xa8\xe0\xaa\xae\xe0\xaa\xb8\xe0\xab\x8d\xe0\xaa\xa4\xe0\xab\x87",
         u"\u0048\u0065\u006c\u006c\u006f\u0020\u007c\u0020\u0048\u00e4\u006a\u0020\u0105\u030a\u0020"
         u"\u0064\u0069\u0067\u0020\u007c\u0020\u0393\u03b5\u03b9\u03ac\u0020\u03c3\u03b1\u03c2\u0020"
         u"\u007c\u0020\u041f\u0440\u0438\u0432\u0435\u0442\u0020\u007c\u0020\u10d2\u10d0\u10db\u10d0"
         u"\u10e0\u10ef\u10dd\u10d1\u10d0\u0020\u007c\u0020\u0bb5\u0ba3\u0b95\u0bcd\u0b95\u0bae\u0bcd"
         u"\u0020\u007c\u0020\u4eca\u65e5\u62dd\u306a\u3073\u3089\u0020\u007c\u0020\u0936\u0941\u092d"
         u"\u0020\u092a\u094d\u092f\u093e\u0930\u0020\u007c\u0020\u10d2\u10d4\u10d2\u10d0\u10ef\u10d2"
         u"\u10d8\u10dc\u10d0\u10e1\u0020\u007c\u0020\uc548\ub155\ud558\uc2ed\ub2c8\uae4c\u0020\u007c"
         u"\u0020\u54c8\u56c9\u0020\u007c\u0020\u0aa8\u0aae\u0ab8\u0acd\u0aa4\u0ac7"},
        {"\xf0\x9f\x98\xb5\xe2\x80\x8d\xf0\x9f\x92\xab\x20\x7c\x20\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d"
         "\xf0\x9f\xa9\xb9\x20\x7c\x20\xf0\x9f\xa9\xb5\x20\x7c\x20\xf0\x9f\x91\x81\xef\xb8\x8f\xe2\x80"
         "\x8d\xf0\x9f\x97\xa8\xef\xb8\x8f\x20\x7c\x20\xf0\x9f\xa7\x8e\xe2\x80\x8d\xe2\x99\x82\xef\xb8"
         "\x8f\xe2\x80\x8d\xe2\x9e\xa1\xef\xb8\x8f",
         u"\U0001F635\U0000200D\U0001F4AB | \U00002764\U0000FE0F\U0000200D\U0001FA79 | \U0001FA75 | "
         u"\U0001F441\U0000FE0F\U0000200D\U0001F5E8\U0000FE0F | \U0001F9CE\U0000200D\U00002642\U0000FE0F"
         u"\U0000200D\U000027A1\U0000FE0F"},
        {"\x01\x1f\x7F\xc2\x80\xc2\xbf\xd0\xb0\xdf\xbf\xe0\xa0\x80\xe0\xbf\xbf\xe1\x80\x80\xec\xbf\xbf"
         "\xed\x80\x80\xed\x9f\xbf\xee\x80\x80\xef\xb0\xb1\xef\xbf\xbf\xf0\x90\x80\x80\xf0\xbf\xbf\xbf"
         "\xf1\x80\x80\x80\xf2\xbf\x80\xbf\xf3\xbf\xbf\xbf\xf4\x80\x80\x80\xf4\x85\x80\xbf\xf4\x8f\xbf\xbf",
         u"\U00000001\U0000001f\U0000007f\U00000080\U000000bf\U00000430\U000007ff\U00000800\U00000fff"
         u"\U00001000\U0000cfff\U0000d000\U0000d7ff\U0000e000\U0000fc31\U0000ffff\U00010000\U0003ffff"
         u"\U00040000\U000bf03f\U000fffff\U00100000\U0010503f\U0010ffff"},
    };

    const std::vector<std::pair<const char*, const char32_t*>> good_strings_u32 = {
        {"\x48\x65\x6c\x6c\x6f\x20\x7c\x20\x48\xc3\xa4\x6a\x20\xc4\x85\xcc\x8a\x20\x64\x69\x67\x20"
         "\x7c\x20\xce\x93\xce\xb5\xce\xb9\xce\xac\x20\xcf\x83\xce\xb1\xcf\x82\x20\x7c\x20\xd0\x9f"
         "\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\x7c\x20\xe1\x83\x92\xe1\x83\x90\xe1\x83\x9b"
         "\xe1\x83\x90\xe1\x83\xa0\xe1\x83\xaf\xe1\x83\x9d\xe1\x83\x91\xe1\x83\x90\x20\x7c\x20\xe0"
         "\xae\xb5\xe0\xae\xa3\xe0\xae\x95\xe0\xaf\x8d\xe0\xae\x95\xe0\xae\xae\xe0\xaf\x8d\x20\x7c"
         "\x20\xe4\xbb\x8a\xe6\x97\xa5\xe6\x8b\x9d\xe3\x81\xaa\xe3\x81\xb3\xe3\x82\x89\x20\x7c\x20"
         "\xe0\xa4\xb6\xe0\xa5\x81\xe0\xa4\xad\x20\xe0\xa4\xaa\xe0\xa5\x8d\xe0\xa4\xaf\xe0\xa4\xbe"
         "\xe0\xa4\xb0\x20\x7c\x20\xe1\x83\x92\xe1\x83\x94\xe1\x83\x92\xe1\x83\x90\xe1\x83\xaf\xe1"
         "\x83\x92\xe1\x83\x98\xe1\x83\x9c\xe1\x83\x90\xe1\x83\xa1\x20\x7c\x20\xec\x95\x88\xeb\x85"
         "\x95\xed\x95\x98\xec\x8b\xad\xeb\x8b\x88\xea\xb9\x8c\x20\x7c\x20\xe5\x93\x88\xe5\x9b\x89"
         "\x20\x7c\x20\xe0\xaa\xa8\xe0\xaa\xae\xe0\xaa\xb8\xe0\xab\x8d\xe0\xaa\xa4\xe0\xab\x87",
         U"\u0048\u0065\u006c\u006c\u006f\u0020\u007c\u0020\u0048\u00e4\u006a\u0020\u0105\u030a\u0020"
         U"\u0064\u0069\u0067\u0020\u007c\u0020\u0393\u03b5\u03b9\u03ac\u0020\u03c3\u03b1\u03c2\u0020"
         U"\u007c\u0020\u041f\u0440\u0438\u0432\u0435\u0442\u0020\u007c\u0020\u10d2\u10d0\u10db\u10d0"
         U"\u10e0\u10ef\u10dd\u10d1\u10d0\u0020\u007c\u0020\u0bb5\u0ba3\u0b95\u0bcd\u0b95\u0bae\u0bcd"
         U"\u0020\u007c\u0020\u4eca\u65e5\u62dd\u306a\u3073\u3089\u0020\u007c\u0020\u0936\u0941\u092d"
         U"\u0020\u092a\u094d\u092f\u093e\u0930\u0020\u007c\u0020\u10d2\u10d4\u10d2\u10d0\u10ef\u10d2"
         U"\u10d8\u10dc\u10d0\u10e1\u0020\u007c\u0020\uc548\ub155\ud558\uc2ed\ub2c8\uae4c\u0020\u007c"
         U"\u0020\u54c8\u56c9\u0020\u007c\u0020\u0aa8\u0aae\u0ab8\u0acd\u0aa4\u0ac7"},
        {"\xf0\x9f\x98\xb5\xe2\x80\x8d\xf0\x9f\x92\xab\x20\x7c\x20\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d"
         "\xf0\x9f\xa9\xb9\x20\x7c\x20\xf0\x9f\xa9\xb5\x20\x7c\x20\xf0\x9f\x91\x81\xef\xb8\x8f\xe2\x80"
         "\x8d\xf0\x9f\x97\xa8\xef\xb8\x8f\x20\x7c\x20\xf0\x9f\xa7\x8e\xe2\x80\x8d\xe2\x99\x82\xef\xb8"
         "\x8f\xe2\x80\x8d\xe2\x9e\xa1\xef\xb8\x8f",
         U"\U0001F635\U0000200D\U0001F4AB | \U00002764\U0000FE0F\U0000200D\U0001FA79 | \U0001FA75 | "
         U"\U0001F441\U0000FE0F\U0000200D\U0001F5E8\U0000FE0F | \U0001F9CE\U0000200D\U00002642\U0000FE0F"
         U"\U0000200D\U000027A1\U0000FE0F"},
        {"\x01\x1f\x7F\xc2\x80\xc2\xbf\xd0\xb0\xdf\xbf\xe0\xa0\x80\xe0\xbf\xbf\xe1\x80\x80\xec\xbf\xbf"
         "\xed\x80\x80\xed\x9f\xbf\xee\x80\x80\xef\xb0\xb1\xef\xbf\xbf\xf0\x90\x80\x80\xf0\xbf\xbf\xbf"
         "\xf1\x80\x80\x80\xf2\xbf\x80\xbf\xf3\xbf\xbf\xbf\xf4\x80\x80\x80\xf4\x85\x80\xbf\xf4\x8f\xbf\xbf",
         U"\U00000001\U0000001f\U0000007f\U00000080\U000000bf\U00000430\U000007ff\U00000800\U00000fff"
         U"\U00001000\U0000cfff\U0000d000\U0000d7ff\U0000e000\U0000fc31\U0000ffff\U00010000\U0003ffff"
         U"\U00040000\U000bf03f\U000fffff\U00100000\U0010503f\U0010ffff"},
    };

    const std::vector<std::pair<const char*, const wchar_t*>> good_strings_w = {
        {"\x48\x65\x6c\x6c\x6f\x20\x7c\x20\x48\xc3\xa4\x6a\x20\xc4\x85\xcc\x8a\x20\x64\x69\x67\x20"
         "\x7c\x20\xce\x93\xce\xb5\xce\xb9\xce\xac\x20\xcf\x83\xce\xb1\xcf\x82\x20\x7c\x20\xd0\x9f"
         "\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\x7c\x20\xe1\x83\x92\xe1\x83\x90\xe1\x83\x9b"
         "\xe1\x83\x90\xe1\x83\xa0\xe1\x83\xaf\xe1\x83\x9d\xe1\x83\x91\xe1\x83\x90\x20\x7c\x20\xe0"
         "\xae\xb5\xe0\xae\xa3\xe0\xae\x95\xe0\xaf\x8d\xe0\xae\x95\xe0\xae\xae\xe0\xaf\x8d\x20\x7c"
         "\x20\xe4\xbb\x8a\xe6\x97\xa5\xe6\x8b\x9d\xe3\x81\xaa\xe3\x81\xb3\xe3\x82\x89\x20\x7c\x20"
         "\xe0\xa4\xb6\xe0\xa5\x81\xe0\xa4\xad\x20\xe0\xa4\xaa\xe0\xa5\x8d\xe0\xa4\xaf\xe0\xa4\xbe"
         "\xe0\xa4\xb0\x20\x7c\x20\xe1\x83\x92\xe1\x83\x94\xe1\x83\x92\xe1\x83\x90\xe1\x83\xaf\xe1"
         "\x83\x92\xe1\x83\x98\xe1\x83\x9c\xe1\x83\x90\xe1\x83\xa1\x20\x7c\x20\xec\x95\x88\xeb\x85"
         "\x95\xed\x95\x98\xec\x8b\xad\xeb\x8b\x88\xea\xb9\x8c\x20\x7c\x20\xe5\x93\x88\xe5\x9b\x89"
         "\x20\x7c\x20\xe0\xaa\xa8\xe0\xaa\xae\xe0\xaa\xb8\xe0\xab\x8d\xe0\xaa\xa4\xe0\xab\x87",
         L"\u0048\u0065\u006c\u006c\u006f\u0020\u007c\u0020\u0048\u00e4\u006a\u0020\u0105\u030a\u0020"
         L"\u0064\u0069\u0067\u0020\u007c\u0020\u0393\u03b5\u03b9\u03ac\u0020\u03c3\u03b1\u03c2\u0020"
         L"\u007c\u0020\u041f\u0440\u0438\u0432\u0435\u0442\u0020\u007c\u0020\u10d2\u10d0\u10db\u10d0"
         L"\u10e0\u10ef\u10dd\u10d1\u10d0\u0020\u007c\u0020\u0bb5\u0ba3\u0b95\u0bcd\u0b95\u0bae\u0bcd"
         L"\u0020\u007c\u0020\u4eca\u65e5\u62dd\u306a\u3073\u3089\u0020\u007c\u0020\u0936\u0941\u092d"
         L"\u0020\u092a\u094d\u092f\u093e\u0930\u0020\u007c\u0020\u10d2\u10d4\u10d2\u10d0\u10ef\u10d2"
         L"\u10d8\u10dc\u10d0\u10e1\u0020\u007c\u0020\uc548\ub155\ud558\uc2ed\ub2c8\uae4c\u0020\u007c"
         L"\u0020\u54c8\u56c9\u0020\u007c\u0020\u0aa8\u0aae\u0ab8\u0acd\u0aa4\u0ac7"},
        {"\xf0\x9f\x98\xb5\xe2\x80\x8d\xf0\x9f\x92\xab\x20\x7c\x20\xe2\x9d\xa4\xef\xb8\x8f\xe2\x80\x8d"
         "\xf0\x9f\xa9\xb9\x20\x7c\x20\xf0\x9f\xa9\xb5\x20\x7c\x20\xf0\x9f\x91\x81\xef\xb8\x8f\xe2\x80"
         "\x8d\xf0\x9f\x97\xa8\xef\xb8\x8f\x20\x7c\x20\xf0\x9f\xa7\x8e\xe2\x80\x8d\xe2\x99\x82\xef\xb8"
         "\x8f\xe2\x80\x8d\xe2\x9e\xa1\xef\xb8\x8f",
         L"\U0001F635\U0000200D\U0001F4AB | \U00002764\U0000FE0F\U0000200D\U0001FA79 | \U0001FA75 | "
         L"\U0001F441\U0000FE0F\U0000200D\U0001F5E8\U0000FE0F | \U0001F9CE\U0000200D\U00002642\U0000FE0F"
         L"\U0000200D\U000027A1\U0000FE0F"},
        {"\x01\x1f\x7F\xc2\x80\xc2\xbf\xd0\xb0\xdf\xbf\xe0\xa0\x80\xe0\xbf\xbf\xe1\x80\x80\xec\xbf\xbf"
         "\xed\x80\x80\xed\x9f\xbf\xee\x80\x80\xef\xb0\xb1\xef\xbf\xbf\xf0\x90\x80\x80\xf0\xbf\xbf\xbf"
         "\xf1\x80\x80\x80\xf2\xbf\x80\xbf\xf3\xbf\xbf\xbf\xf4\x80\x80\x80\xf4\x85\x80\xbf\xf4\x8f\xbf\xbf",
         L"\U00000001\U0000001f\U0000007f\U00000080\U000000bf\U00000430\U000007ff\U00000800\U00000fff"
         L"\U00001000\U0000cfff\U0000d000\U0000d7ff\U0000e000\U0000fc31\U0000ffff\U00010000\U0003ffff"
         L"\U00040000\U000bf03f\U000fffff\U00100000\U0010503f\U0010ffff"},
    };

    const std::vector<std::pair<const char*, const char16_t*>> bad_u8_to_u16_strings = {
        {"\x80", u"\ufffd"},
        {"\x80\x60", u"\ufffd\x60"},
        {"\xc1", u"\ufffd"},
        {"\xc1\x80", u"\ufffd\ufffd"},
        {"\xc1\x20", u"\ufffd "},
        {"\xc2\x7f", u"\ufffd\x7f"},
        {"\xe0\x9f", u"\ufffd\ufffd"},
        {"\xe0\x3f", u"\ufffd\x3f"},
        {"\xe0\xa0\x20", u"\ufffd "},
        {"\xe1\x80\x7f", u"\ufffd\x7f"},
        {"\xed\x7f\x80", u"\ufffd\x7f\ufffd"},
        {"\xed\x9f\x20", u"\ufffd "},
        {"\xee\x80\xc0", u"\ufffd\ufffd"},
        {"\xee\x80\x7f", u"\ufffd\x7f"},
        {"\xf0\x90\x80\xff", u"\ufffd\ufffd"},
        {"\xf0\x90\x7f\x20", u"\ufffd\x7f\x20"},
        {"\xf1\x80\x80\xc2\x80", u"\ufffd\x80"},
        {"\xf1\xf1\x80\x80\xaa\x80", u"\ufffd\U0004002a\ufffd"},
        {"\xf4\x80\xbf\xed\x80\x80", u"\ufffd\ud000"},
        {"\xf5\x30\xf6\xc2\xbf", u"\ufffd\x30\ufffd\xbf"},
        {"\xc0\xaf\xe0\x80\xbf\xf0\x81\x82\x41", u"\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\x41"},
        {"\xed\xa0\x80\xed\xbf\xbf\xed\xaf\x41", u"\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd\x41"},
        {"\xf4\x91\x92\x93\xff\x41\x80\xbf\x42", u"\ufffd\ufffd\ufffd\ufffd\ufffd\x41\ufffd\ufffd\x42"},
        {"\xe1\x80\xe2\xf0\x91\x92\xf1\xbf\x41", u"\ufffd\ufffd\ufffd\ufffd\x41"},
    };

    const std::vector<std::pair<std::u16string, const char*>> bad_u16_to_u8_strings = {
        {{0xdbff, 0xe000, 0x20}, "\xef\xbf\xbd\xee\x80\x80 "},
        {{0xdc80, 0x20, 0xdcff}, "\xef\xbf\xbd \xef\xbf\xbd"},
        {{0xdddd}, "\xef\xbf\xbd"},
        {{0xd800}, "\xef\xbf\xbd"},
        {{0xd7ff, 0xe000, 0xd800, 0xdfff, 0xdbff, 0xdc00, 0xd900, 0xda00, 0xdcaa, 0x7f, 0x7ff, 0xffff, 0xdc00, 0xdaaa},
         "\xed\x9f\xbf\xee\x80\x80\xf0\x90\x8f\xbf\xf4\x8f\xb0\x80\xef\xbf\xbd\xf2\x90\x82\xaa\x7f"
         "\xdf\xbf\xef\xbf\xbf\xef\xbf\xbd\xef\xbf\xbd"},
    };

    const std::vector<std::pair<std::u32string, const char*>> bad_u32_to_u8_strings = {
        {{0xdbff, 0xe000, 0x20}, "\xef\xbf\xbd\xee\x80\x80 "},
        {{0xdc80, 0x20, 0xdcff}, "\xef\xbf\xbd \xef\xbf\xbd"},
        {{0xdddd}, "\xef\xbf\xbd"},
        {{0xd800}, "\xef\xbf\xbd"},
        {{0xd7ff, 0xe000, 0x10ffff, 0xd800, 0xdfff, 0xdbff, 0xdc00, 0xd900, 0xda00, 0xdcaa, 0x7f, 0x7ff, 0xffff, 0xdc00, 0xdaaa, 0xffffffff,
          0x110000},
         "\xed\x9f\xbf\xee\x80\x80\xf4\x8f\xbf\xbf\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"
         "\xef\xbf\xbd\xef\xbf\xbd\x7f\xdf\xbf\xef\xbf\xbf\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"},
    };

    TEST(UnicodeConversionsTest, test_u16_to_u8)
    {
        for (const auto& [u8_str, u16_str] : good_strings)
        {
            const auto u8_conv = u16_to_u8(u16_str);
            const auto u8_byte_len = std::strlen(u8_str);

            EXPECT_TRUE(u8_byte_len == u8_conv.length() && memcmp(u8_str, u8_conv.data(), u8_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u32_to_u8)
    {
        for (const auto& [u8_str, u32_str] : good_strings_u32)
        {
            const auto u8_conv = u32_to_u8(u32_str);
            const auto u8_byte_len = std::strlen(u8_str);

            EXPECT_TRUE(u8_byte_len == u8_conv.length() && memcmp(u8_str, u8_conv.data(), u8_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_w_to_u8)
    {
        for (const auto& [u8_str, w_str] : good_strings_w)
        {
            const auto u8_conv = w_to_u8(w_str);
            const auto u8_byte_len = std::strlen(u8_str);

            EXPECT_TRUE(u8_byte_len == u8_conv.length() && memcmp(u8_str, u8_conv.data(), u8_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u8_to_u16)
    {
        for (const auto& [u8_str, u16_str] : good_strings)
        {
            const auto u16_conv = u8_to_u16(u8_str);
            const auto u16_byte_len = std::char_traits<char16_t>::length(u16_str) * 2;

            EXPECT_TRUE(u16_byte_len == u16_conv.length() * 2 && memcmp(u16_str, u16_conv.data(), u16_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u8_to_u32)
    {
        for (const auto& [u8_str, u32_str] : good_strings_u32)
        {
            const auto u32_conv = u8_to_u32(u8_str);
            const auto u32_byte_len = std::char_traits<char32_t>::length(u32_str) * sizeof(char32_t);

            EXPECT_TRUE(u32_byte_len == u32_conv.length() * sizeof(char32_t) && memcmp(u32_str, u32_conv.data(), u32_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u8_to_w)
    {
        for (const auto& [u8_str, w_str] : good_strings_w)
        {
            const auto w_conv = u8_to_w(u8_str);
            const auto w_byte_len = std::char_traits<wchar_t>::length(w_str) * sizeof(wchar_t);

            EXPECT_TRUE(w_byte_len == w_conv.length() * sizeof(wchar_t) && memcmp(w_str, w_conv.data(), w_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u8_to_u16_bad)
    {
        for (const auto& [u8_str, u16_str] : bad_u8_to_u16_strings)
        {
            const auto u16_conv = u8_to_u16(u8_str);
            const auto u16_byte_len = std::char_traits<char16_t>::length(u16_str) * 2;

            EXPECT_TRUE(u16_byte_len == u16_conv.length() * 2 && memcmp(u16_str, u16_conv.data(), u16_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u16_to_u8_bad)
    {
        for (const auto& [u16_str, u8_str] : bad_u16_to_u8_strings)
        {
            const auto u8_conv = u16_to_u8(u16_str);
            const auto u8_byte_len = std::strlen(u8_str);

            EXPECT_TRUE(u8_byte_len == u8_conv.length() && memcmp(u8_str, u8_conv.data(), u8_byte_len) == 0);
        }
    }

    TEST(UnicodeConversionsTest, test_u32_to_u8_bad)
    {
        for (const auto& [u32_str, u8_str] : bad_u32_to_u8_strings)
        {
            const auto u8_conv = u32_to_u8(u32_str);
            const auto u8_byte_len = std::strlen(u8_str);

            EXPECT_TRUE(u8_byte_len == u8_conv.length() && memcmp(u8_str, u8_conv.data(), u8_byte_len) == 0);
        }
    }
}
