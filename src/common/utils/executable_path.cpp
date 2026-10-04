#include "executable_path.hpp"

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <stdexcept>
#include <vector>

namespace sogen::utils
{
    std::filesystem::path get_current_executable_path()
    {
#if defined(_WIN32)
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;)
        {
            const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                throw std::runtime_error("Resolving module file name failed");
            }
            if (length < buffer.size())
            {
                return std::filesystem::path{buffer.data(), buffer.data() + length};
            }
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        uint32_t size{};
        (void)_NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            throw std::runtime_error("Resolving executable path failed");
        }
        return std::filesystem::weakly_canonical(buffer.data());
#elif defined(__linux__) || defined(__ANDROID__)
        return std::filesystem::canonical("/proc/self/exe");
#else
        return std::filesystem::current_path();
#endif
    }
}
