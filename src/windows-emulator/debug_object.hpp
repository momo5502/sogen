#pragma once

#include "handles.hpp"

namespace sogen
{
    class windows_emulator;
    struct debug_object;

    // Takes the next event one of the debug object's child processes reported and writes it to state_change as a
    // DBGUI_WAIT_STATE_CHANGE, creating the handles the debugger receives with it. False when none is pending.
    bool deliver_debug_event(windows_emulator& win_emu, handle debug_handle, uint64_t state_change);

    // What closing the last handle to a debug object does: DEBUG_KILL_ON_CLOSE, which DbgUi sets, terminates the
    // debuggees, and without it they are detached and keep running.
    void release_debug_object(windows_emulator& win_emu, debug_object& object);
}
