#include "std_include.hpp"
#include "logger.hpp"

#include <utils/finally.hpp>

namespace
{
#ifdef _WIN32
#define COLOR(win, posix) win
    using color_type = WORD;
#else
#define COLOR(win, posix) posix
    using color_type = const char*;
#endif

    color_type get_reset_color()
    {
        return COLOR(7, "\033[0m");
    }

    color_type get_color_type(const color c)
    {
        using enum color;

        switch (c)
        {
        case black:
            return COLOR(0x8, "\033[0;90m");
        case red:
            return COLOR(0xC, "\033[0;91m");
        case green:
            return COLOR(0xA, "\033[0;92m");
        case yellow:
            return COLOR(0xE, "\033[0;93m");
        case blue:
            return COLOR(0x9, "\033[0;94m");
        case cyan:
            return COLOR(0xB, "\033[0;96m");
        case pink:
            return COLOR(0xD, "\033[0;95m");
        case white:
            return COLOR(0xF, "\033[0;97m");
        case dark_gray:
            return COLOR(0x8, "\033[0;97m");
        case gray:
        default:
            return get_reset_color();
        }
    }

#ifdef _WIN32
    HANDLE get_console_handle()
    {
        return GetStdHandle(STD_OUTPUT_HANDLE);
    }
#endif

    void set_color(const color_type color)
    {
#ifdef _WIN32
        SetConsoleTextAttribute(get_console_handle(), color);
#else
        printf("%s", color);
#endif
    }

    void reset_color()
    {
        (void)fflush(stdout);
        set_color(get_reset_color());
        (void)fflush(stdout);
    }

    std::string_view format(va_list* ap, const char* message)
    {
        thread_local char buffer[0x1000];

#ifdef _WIN32
        const int count = _vsnprintf_s(buffer, sizeof(buffer), sizeof(buffer), message, *ap);
#else
        const int count = vsnprintf(buffer, sizeof(buffer), message, *ap);
#endif

        if (count < 0)
            return {};
        return {buffer, static_cast<size_t>(count)};
    }

#define format_to_string(msg, str)     \
    va_list ap;                        \
    va_start(ap, msg);                 \
    const auto str = format(&ap, msg); \
    va_end(ap);

    void print_colored(const std::string_view& line, const color_type base_color)
    {
        const auto _ = utils::finally(&reset_color);
        set_color(base_color);
        (void)fwrite(line.data(), 1, line.size(), stdout);
    }
}

void logger::print(const color c, const std::string_view message) const
{
    if (this->disable_output_)
    {
        return;
    }

    print_colored(message, get_color_type(c));
}

void logger::print(const color c, const char* message, ...) const
{
    format_to_string(message, data);
    this->print(c, data);
}

void logger::info(const char* message, ...) const
{
    format_to_string(message, data);
    this->print(color::cyan, data);
}

void logger::warn(const char* message, ...) const
{
    format_to_string(message, data);
    this->print(color::yellow, data);
}

void logger::error(const char* message, ...) const
{
    format_to_string(message, data);
    this->print(color::red, data);
}

void logger::success(const char* message, ...) const
{
    format_to_string(message, data);
    this->print(color::green, data);
}

void logger::log(const char* message, ...) const
{
    format_to_string(message, data);
    this->print(color::gray, data);
}
