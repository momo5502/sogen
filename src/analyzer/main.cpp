#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sogen
{
    int windows_main(int argc, char** argv);
    int linux_main(int argc, char** argv);
}

namespace
{
    bool should_use_linux_analyzer()
    {
        const char* value = std::getenv("EMULATOR_LINUX");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty())
        return {};

    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);

    std::string out(size, '\0');

    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), size, nullptr, nullptr);

    return out;
}

int dispatch_main(int argc, wchar_t** wargv)
{
    std::vector<std::string> utf8_storage;
    utf8_storage.reserve(argc);

    for (int i = 0; i < argc; ++i)
    {
        utf8_storage.push_back(wide_to_utf8(wargv[i]));
    }

    std::vector<char*> argv_utf8;
    argv_utf8.reserve(argc + 1);

    for (auto& arg : utf8_storage)
    {
        argv_utf8.push_back(arg.data());
    }

    argv_utf8.push_back(nullptr);

    if (should_use_linux_analyzer())
    {
        return sogen::linux_main(argc, argv_utf8.data());
    }

    return sogen::windows_main(argc, argv_utf8.data());
}

int wmain(int argc, wchar_t** argv)
{
    return dispatch_main(argc, argv);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    return dispatch_main(__argc, __wargv);
}
#else
int dispatch_main(int argc, char** argv)
{
    if (should_use_linux_analyzer())
    {
        return sogen::linux_main(argc, argv);
    }

    return sogen::windows_main(argc, argv);
}

int main(int argc, char** argv)
{
    return dispatch_main(argc, argv);
}
#endif
