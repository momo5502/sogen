#pragma once

#include "../macos_syscall_context.hpp"

namespace sogen
{
    void sys_exit(const macos_syscall_context& c);
    void sys_write(const macos_syscall_context& c);
    void sys_mprotect(const macos_syscall_context& c);
}
