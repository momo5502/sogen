#pragma once

#include "../std_include.hpp"
#include "macos_native_dispatch.hpp"

namespace sogen
{
    // The Process Manager's serial number for the emulated process. Any value above kSystemProcess (1)
    // and kCurrentProcess (2) is a real one; nothing may assume a particular number, and every routine
    // below hands out this one so a guest that round-trips it keeps matching.
    constexpr uint32_t MACOS_PROCESS_SERIAL_NUMBER_HIGH = 0;
    constexpr uint32_t MACOS_PROCESS_SERIAL_NUMBER_LOW = 0x1001;

    void register_process_manager_routines(macos_native_dispatch& dispatch);
}
