#include "os_selection.hpp"

#include <cstdlib>

namespace sogen
{
    int windows_main(int argc, char** argv);
    int linux_main(int argc, char** argv);
    int macos_main(int argc, char** argv);
}

int main(int argc, char** argv)
{
    auto invocation = sogen::select_analyzer_invocation(argc, argv, std::getenv("EMULATOR_LINUX"), std::getenv("EMULATOR_MACOS"));

    const auto count = static_cast<int>(invocation.arguments.size());
    auto** arguments = invocation.arguments.data();

    switch (invocation.os)
    {
    case sogen::analyzer_os::linux:
        return sogen::linux_main(count, arguments);
    case sogen::analyzer_os::macos:
        return sogen::macos_main(count, arguments);
    case sogen::analyzer_os::windows:
    default:
        return sogen::windows_main(count, arguments);
    }
}
