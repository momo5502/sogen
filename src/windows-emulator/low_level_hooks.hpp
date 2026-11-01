#pragma once

#include "syscall_utils.hpp"

namespace sogen
{
    // WH_KEYBOARD_LL/WH_MOUSE_LL hooks run on the thread that installed them, while it retrieves or waits for
    // messages. Message syscalls call this first: it dispatches the next hook pending on the calling thread and
    // returns true, and the syscall's completion re-enters the syscall once the hook returned.
    bool dispatch_low_level_hook(const syscall_context& c, callback_id completion_id);
    void complete_low_level_hook(const syscall_context& c);

    // A WH_GETMESSAGE hook of the calling thread sees each message GetMessage/PeekMessage retrieves, and may change it.
    // The completion returns the message the hook left.
    bool dispatch_get_message_hook(const syscall_context& c, callback_id completion_id, const msg& message, UINT remove_flags);
    msg complete_get_message_hook(const syscall_context& c);
}
