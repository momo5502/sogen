#include "string.hpp"

#include <array>
#include <cstdarg>

namespace utils::string
{
    // NOLINTNEXTLINE(cert-dcl50-cpp)
    const char* va(const char* format, ...)
    {
        constexpr auto buffer_count = 4;
        thread_local std::array<std::vector<char>, buffer_count> buffers{};
        thread_local size_t current_index{0};

        const auto index = current_index++;
        current_index %= buffers.size();

        auto& buffer = buffers.at(index);

        if (buffer.size() < 10)
        {
            buffer.resize(10);
        }

        while (true)
        {
            va_list ap{};
            va_start(ap, format);

#ifdef _WIN32
            const int res = vsnprintf_s(buffer.data(), buffer.size(), _TRUNCATE, format, ap);
#else
            const int res = vsnprintf(buffer.data(), buffer.size(), format, ap);
#endif

            va_end(ap);

            if (res > 0 && static_cast<size_t>(res) < buffer.size())
            {
                break;
            }
            if (res == 0)
            {
                return nullptr;
            }

            buffer.resize(std::max(buffer.size() * 2, static_cast<size_t>(1)));
        }

        return buffer.data();
    }
}
