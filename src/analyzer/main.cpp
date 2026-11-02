#include <cstdlib>

namespace sogen
{
    int windows_main(int argc, char** argv);
    int linux_main(int argc, char** argv);
    int macos_main(int argc, char** argv);
}

namespace
{
    bool is_env_flag_set(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }
}

int main(int argc, char** argv)
{
    if (is_env_flag_set("EMULATOR_LINUX"))
    {
        return sogen::linux_main(argc, argv);
    }

    if (is_env_flag_set("EMULATOR_MACOS"))
    {
        return sogen::macos_main(argc, argv);
    }

    return sogen::windows_main(argc, argv);
}
