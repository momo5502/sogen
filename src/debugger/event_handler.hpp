#pragma once

#include <windows_emulator.hpp>

namespace debugger
{
    enum class emulation_state
    {
        none,
        running,
        paused,
    };

    struct event_context
    {
        windows_emulator& win_emu;
        emulation_state state{emulation_state::none};
    };

    void handle_events(event_context& c);
    void handle_exit(const windows_emulator& win_emu, std::optional<NTSTATUS> exit_status);
    void update_emulation_status(const windows_emulator& win_emu);

    // --- Phase 3: breakpoint / step control ---
    //
    // The break loop reuses the exact pause primitive PauseRequest uses
    // (block, drain events, resume) but is entered precisely from a
    // breakpoint's execute hook, so it cannot desync the emulator and needs
    // no changes to the analyzer run loop.

    enum class step_request
    {
        none,    // stay paused
        cont,    // resume freely
        into,    // execute exactly one instruction
        over,    // step, run calls to completion
        step_out // run until current function returns
    };

    // Called from a debug_session execute hook when a breakpoint (or an armed
    // step) is hit. Blocks the emulation thread at `address` until a resume /
    // step command arrives, then arms the next step and returns so emulation
    // continues. No-op if not running under the browser event channel.
    void enter_breakpoint(windows_emulator& win_emu, uint64_t address);

    // Set by the DebugCommand step/continue handlers (and RunRequest) to
    // release a blocked break loop with the requested motion.
    void request_resume(step_request request, uint64_t run_to_address = 0);
}
