#include "std_include.hpp"
#include "event_manager.hpp"
#include "windows_emulator.hpp"

void syscall_event::print(generic_logger& log) const
{
    auto& win = *this->win_emu;
    auto& emu = win.emu();

    const auto address = emu.read_instruction_pointer();
    const auto* mod = win.mod_manager.find_by_address(address);

    if (mod != win.mod_manager.ntdll && mod != win.mod_manager.win32u)
    {
        log.print(color::blue, "Executing inline syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s)\n",
                  static_cast<int>(this->data.name.size()), this->data.name.data(), this->data.id, address,
                  mod ? mod->name.c_str() : "<N/A>");
    }
    else
    {
        if (mod->is_within(win_emu->process.previous_ip))
        {
            const auto rsp = emu.read_stack_pointer();

            uint64_t return_address{};
            emu.try_read_memory(rsp, &return_address, sizeof(return_address));

            const auto* caller_mod_name = win.mod_manager.find_name(return_address);

            log.print(color::dark_gray, "Executing syscall: %.*s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                      static_cast<int>(this->data.name.size()), this->data.name.data(), this->data.id, address,
                      return_address, caller_mod_name);
        }
        else
        {
            const auto* previous_mod = win.mod_manager.find_by_address(win.process.previous_ip);

            log.print(color::blue,
                      "Crafted out-of-line syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                      static_cast<int>(this->data.name.size()), this->data.name.data(), this->data.id, address,
                      mod ? mod->name.c_str() : "<N/A>", win.process.previous_ip,
                      previous_mod ? previous_mod->name.c_str() : "<N/A>");
        }
    }
}
