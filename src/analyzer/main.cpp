#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <emulator-platform/platform/platform.hpp>

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
int dispatch_wmain(int argc, wchar_t** wargv)
{
    std::vector<std::string> utf8_storage;
    utf8_storage.reserve(argc);

    for (int i = 0; i < argc; ++i)
    {
        utf8_storage.push_back(sogen::w_to_u8(wargv[i]));
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

int dispatch_current_command_line()
{
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
    {
        return 1;
    }

    const auto result = dispatch_wmain(argc, argv);
    LocalFree(argv);
    return result;
}
#endif

#ifndef _WIN32
int dispatch_main(int argc, char** argv)
{
    if (should_use_linux_analyzer())
    {
        return sogen::linux_main(argc, argv);
    }

    return sogen::windows_main(argc, argv);
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
    return dispatch_current_command_line();
#else
    return dispatch_main(argc, argv);
#endif
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return dispatch_current_command_line();
}
#endif
