#include "../std_include.hpp"
#include "buffered_console_backend.hpp"

#include <algorithm>
#include <limits>

#if !defined(OS_WINDOWS) && !defined(OS_EMSCRIPTEN)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace sogen
{
    console_size buffered_console_backend::get_size() const
    {
#ifdef OS_WINDOWS
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        {
            return {};
        }
        const auto width = info.srWindow.Right - info.srWindow.Left + 1;
        const auto height = info.srWindow.Bottom - info.srWindow.Top + 1;
        constexpr auto maximum = std::numeric_limits<int16_t>::max();
        if (width <= 0 || height <= 0 || width > maximum || height > maximum)
        {
            return {};
        }
        return {.width = static_cast<int16_t>(width), .height = static_cast<int16_t>(height)};
#elif !defined(OS_EMSCRIPTEN)
        winsize size{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0 || size.ws_row == 0)
        {
            return {};
        }
        constexpr auto maximum = std::numeric_limits<int16_t>::max();
        return {.width = static_cast<int16_t>(std::min<int>(size.ws_col, maximum)),
                .height = static_cast<int16_t>(std::min<int>(size.ws_row, maximum))};
#else
        return {};
#endif
    }
}
