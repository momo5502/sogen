#pragma once
#include <utils/object.hpp>

#if defined(__clang__) || defined(__GNUC__)
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos) __attribute__((format(printf, fmt_pos, var_pos)))
#else
#define FORMAT_ATTRIBUTE(fmt_pos, var_pos)
#endif

enum class color
{
    black,
    red,
    green,
    yellow,
    blue,
    cyan,
    pink,
    white,
    gray,
    dark_gray,
};

struct generic_logger : utils::object
{
    virtual void print(color c, std::string_view message) = 0;
    virtual void print(color c, const char* message, ...) FORMAT_ATTRIBUTE(3, 4) = 0;
};
