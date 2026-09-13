#include "../std_include.hpp"
#include "mig_kernel_servers.hpp"

#include "../macos_emulator.hpp"

namespace sogen::mach
{
    namespace
    {
        // task_restartable is not in any shipped .defs file; the ranges libpthread registers are only
        // consulted when the kernel interrupts a critical section, which a single-threaded emulator never
        // does. Accepting without parsing them is honest -- refusing would fail a call that succeeds on
        // real Darwin, and parsing them would be dead code.
        //
        // Deliberately does NOT require an NDR record: routine 8001 marshals no arguments, so MIG omits
        // it, and a decoder that demands one answers a valid request with MIG_BAD_ARGUMENTS.
        std::vector<uint8_t> restartable_success(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(kr::success));
            return builder.finish();
        }
    }

    void register_restartable_routines(mig_server_table& table)
    {
        table.register_routine(kernel_object_kind::task, 8000, restartable_success, "task_restartable_ranges_register");
        table.register_routine(kernel_object_kind::task, 8001, restartable_success, "task_restartable_ranges_synchronize");
    }
}
