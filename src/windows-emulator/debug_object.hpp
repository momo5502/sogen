#pragma once

#include "handles.hpp"

namespace sogen
{
    class windows_emulator;

    // Takes the next event one of the debug object's child processes reported and writes it to state_change as a
    // DBGUI_WAIT_STATE_CHANGE, creating the handles the debugger receives with it. False when none is pending.
    bool deliver_debug_event(windows_emulator& win_emu, handle debug_handle, uint64_t state_change);
}
