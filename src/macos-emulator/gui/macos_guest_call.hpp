#pragma once

#include "../std_include.hpp"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace sogen
{
    class macos_emulator;

    constexpr size_t MACOS_GUI_MAX_CALL_DEPTH = 8;

    // What a chain started away from a call site has to put back. At a call site AAPCS already says
    // what the caller may still rely on, so nothing needs saving; a guest parked on its own svc is
    // inside a function body instead, with the trap number in x16, the trap's arguments in x0..x7 and
    // pc rewound onto the svc so that resuming re-runs it. The vector bank is deliberately absent: the
    // park sites are leaf trap stubs entered by bl, so v0..v7 and v16..v31 are already dead there, and
    // v8..v15 are preserved by the guest callees the chain runs.
    struct macos_guest_resume_state
    {
        std::array<uint64_t, 29> x{};
        uint64_t fp{};
        uint64_t lr{};
        uint64_t sp{};
        uint64_t nzcv{};
        uint64_t pc{};
    };

    struct macos_guest_call_request
    {
        uint64_t function{};
        std::array<uint64_t, 8> args{};

        // AAPCS puts floating-point arguments in their own bank, so a CGRect -- four doubles by value --
        // travels in d0..d3 and occupies none of the integer slots above. Every slot is written on every
        // call: a callee reads only the ones its prototype declares, so an unused one costs nothing.
        std::array<double, 8> double_args{};

        std::function<void(macos_emulator&, uint64_t result)> on_return{};
    };

    // Lets a native handler call back into the guest. Saving the CPU state and re-entering the emulator
    // is not available: the handler runs inside a unicorn hook, and starting the engine from there is
    // undefined. So a call is a continuation instead -- the register file is rewritten so the guest
    // performs the call itself, with lr pointing at a page of svc that sogen owns. Returning there fires
    // the trap, pops the frame and runs the continuation, which may start another call.
    class macos_guest_call_stack
    {
      public:
        bool prepare(macos_emulator& emu);

        bool active() const
        {
            return !this->frames_.empty();
        }

        size_t depth() const
        {
            return this->frames_.size();
        }

        bool begin(macos_emulator& emu, macos_guest_call_request request);
        bool is_trap(uint64_t entry) const;
        bool handle_trap(macos_emulator& emu, uint64_t entry);

        // Snapshots the guest so that the chain started next unwinds by restoring it rather than by
        // returning to lr. Refuses while a chain is already in flight, because the snapshot would then
        // be of a callee rather than of the interrupted guest. disarm_resume undoes it when nothing
        // started after all.
        bool arm_resume(macos_emulator& emu);
        void disarm_resume();

        void reset();

      private:
        struct frame
        {
            std::function<void(macos_emulator&, uint64_t result)> on_return{};

            // Where the guest goes once this frame's callee and every call its continuation starts have
            // returned. Per frame rather than per chain, because a native handler may be entered from
            // guest code while another chain is already in flight: its own caller is waiting at a
            // different address, and one address for the whole stack would strand it.
            uint64_t return_address{};
        };

        std::vector<frame> frames_{};

        // The return address a continuation inherits. A continuation runs after its own frame has been
        // popped, with lr still pointing at the trap page, so a call it starts cannot read its return
        // address out of lr -- it belongs to the frame that just went away.
        uint64_t inherited_return_{0};

        std::optional<macos_guest_resume_state> resume_{};

        bool prepared_{false};
    };
}
