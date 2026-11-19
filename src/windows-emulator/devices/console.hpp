#pragma once

#include "../io_device.hpp"

namespace sogen
{

    std::unique_ptr<io_device> create_console_device(const device_creation_context& context);
    NTSTATUS read_console_input(windows_emulator& win_emu, const io_device_context& context);

} // namespace sogen
