#include "analysis.hpp"
#include "windows_emulator.hpp"
#include "utils/lazy_object.hpp"

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
        win_emu.log.print(color::pink, "Suspicious: %.*s (0x%" PRIx64 ")\n", STR_VIEW_VA(details), rip);
    }

    void handle_instruction(windows_emulator& win_emu, const uint64_t address)
    {
        const auto is_main_exe = win_emu.mod_manager.executable->is_within(address);
        const auto is_previous_main_exe = win_emu.mod_manager.executable->is_within(win_emu.process.previous_ip);

        const auto binary = utils::make_lazy([&] {
            if (is_main_exe)
            {
                return win_emu.mod_manager.executable;
            }

            return win_emu.mod_manager.find_by_address(address); //
        });

        const auto previous_binary = utils::make_lazy([&] {
            if (is_previous_main_exe)
            {
                return win_emu.mod_manager.executable;
            }

            return win_emu.mod_manager.find_by_address(win_emu.process.previous_ip); //
        });

        const auto is_in_interesting_module = [&] {
            if (win_emu.modules_.empty())
            {
                return false;
            }

            return (binary && win_emu.modules_.contains(binary->name)) ||
                   (previous_binary && win_emu.modules_.contains(previous_binary->name));
        };

        const auto is_interesting_call = is_previous_main_exe //
                                         || is_main_exe       //
                                         || is_in_interesting_module();

        if (win_emu.silent_until_main_ && is_main_exe)
        {
            win_emu.silent_until_main_ = false;
            win_emu.log.disable_output(false);
        }

        if (!win_emu.verbose && !win_emu.verbose_calls && !is_interesting_call)
        {
            return;
        }

        if (binary)
        {
            const auto export_entry = binary->address_names.find(address);
            if (export_entry != binary->address_names.end() &&
                !win_emu.ignored_functions_.contains(export_entry->second))
            {
                const auto rsp = win_emu.emu().read_stack_pointer();

                uint64_t return_address{};
                win_emu.emu().try_read_memory(rsp, &return_address, sizeof(return_address));

                const auto* mod_name = win_emu.mod_manager.find_name(return_address);

                win_emu.log.print(is_interesting_call ? color::yellow : color::dark_gray,
                                  "Executing function: %s - %s (0x%" PRIx64 ") via (0x%" PRIx64 ") %s\n",
                                  binary->name.c_str(), export_entry->second.c_str(), address, return_address,
                                  mod_name);
            }
            else if (address == binary->entry_point)
            {
                win_emu.log.print(is_interesting_call ? color::yellow : color::gray,
                                  "Executing entry point: %s (0x%" PRIx64 ")\n", binary->name.c_str(), address);
            }
        }

        if (!win_emu.verbose)
        {
            return;
        }

        auto& emu = win_emu.emu();

        // TODO: Remove or cleanup
        win_emu.log.print(
            color::gray,
            "Inst: %16" PRIx64 " - RAX: %16" PRIx64 " - RBX: %16" PRIx64 " - RCX: %16" PRIx64 " - RDX: %16" PRIx64
            " - R8: %16" PRIx64 " - R9: %16" PRIx64 " - RDI: %16" PRIx64 " - RSI: %16" PRIx64 " - %s\n",
            address, emu.reg(x86_register::rax), emu.reg(x86_register::rbx), emu.reg(x86_register::rcx),
            emu.reg(x86_register::rdx), emu.reg(x86_register::r8), emu.reg(x86_register::r9),
            emu.reg(x86_register::rdi), emu.reg(x86_register::rsi), binary ? binary->name.c_str() : "<N/A>");
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
    cb.on_instruction = make_callback(win_emu, handle_instruction);
    cb.on_suspicious_activity = make_callback(win_emu, handle_suspicious_activity);
}
