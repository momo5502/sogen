#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace sogen
{

    namespace syscalls
    {
        NTSTATUS handle_NtUnBindCompositionSurface()
        {
            return STATUS_SUCCESS;
        }
    }

} // namespace sogen
