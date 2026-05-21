#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
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

int main(int argc, char** argv)
{
    if (should_use_linux_analyzer())
    {
        return sogen::linux_main(argc, argv);
    }

    return sogen::windows_main(argc, argv);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return main(__argc, __argv);
}
#endif
