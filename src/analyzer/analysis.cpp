#include "analysis.hpp"
#include "windows_emulator.hpp"

#define STR_VIEW_VA(str) static_cast<int>((str).size()), (str).data()

namespace
{
    template <typename Return, typename... Args>
    std::function<Return(Args...)> make_callback(windows_emulator& win_emu,
                                                 Return (*callback)(windows_emulator&, Args...))
    {
        return [&win_emu, callback](Args... args) {
            return callback(win_emu, std::forward<Args>(args)...); //
        };
    }

    void handle_suspicious_activity(windows_emulator& win_emu, const std::string_view details)
    {
        const auto rip = win_emu.emu().read_instruction_pointer();
        win_emu.log.print(color::pink, "Suspicious: %.*s (0x%" PRIX64 ")\n", STR_VIEW_VA(details), rip);
    }

    emulator_callbacks::continuation handle_syscall(windows_emulator& win_emu, const uint32_t syscall_id,
                                                    const std::string_view syscall_name)
    {
        auto& emu = win_emu.emu();

        const auto address = emu.read_instruction_pointer();
        const auto* mod = win_emu.mod_manager.find_by_address(address);
        const auto is_sus_module = mod != win_emu.mod_manager.ntdll && mod != win_emu.mod_manager.win32u;

        if (is_sus_module)
        {
            win_emu.log.print(color::blue, "Executing inline syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s)\n",
                              STR_VIEW_VA(syscall_name), syscall_id, address, mod ? mod->name.c_str() : "<N/A>");
        }
        else if (mod->is_within(win_emu.process.previous_ip))
        {
            const auto rsp = emu.read_stack_pointer();

            uint64_t return_address{};
            emu.try_read_memory(rsp, &return_address, sizeof(return_address));

            const auto* caller_mod_name = win_emu.mod_manager.find_name(return_address);

            win_emu.log.print(color::dark_gray,
                              "Executing syscall: %.*s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                              STR_VIEW_VA(syscall_name), syscall_id, address, return_address, caller_mod_name);
        }
        else
        {
            const auto* previous_mod = win_emu.mod_manager.find_by_address(win_emu.process.previous_ip);

            win_emu.log.print(color::blue,
                              "Crafted out-of-line syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                              STR_VIEW_VA(syscall_name), syscall_id, address, mod ? mod->name.c_str() : "<N/A>",
                              win_emu.process.previous_ip, previous_mod ? previous_mod->name.c_str() : "<N/A>");
        }

        return instruction_hook_continuation::run_instruction;
    }
}

void register_analysis_callbacks(windows_emulator& win_emu)
{
    auto& cb = win_emu.callbacks;

    cb.on_syscall = make_callback(win_emu, handle_syscall);
    cb.on_suspicious_activity = make_callback(win_emu, handle_suspicious_activity);
}
