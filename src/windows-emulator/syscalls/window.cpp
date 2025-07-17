#include "../std_include.hpp"
#include "../emulator_utils.hpp"
#include "../syscall_utils.hpp"

namespace syscalls
{
    NTSTATUS handle_NtUserBuildHwndList(const syscall_context& c, const handle desktop_handle,
                                            const handle parent_handle,
                                            const BOOLEAN is_children, const ULONG thread_id,
                                            const ULONG hwnd, handle hwnd_list, ULONG hwnd_needed)
    {
        return STATUS_SUCCESS;
    }
}