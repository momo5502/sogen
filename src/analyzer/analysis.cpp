#include "std_include.hpp"

#include "analysis.hpp"
#include "windows_emulator.hpp"
#include "utils/lazy_object.hpp"

#define STR_VIEW_VA(str) static_cast<int>((str).size()), (str).data()

namespace
{
    template <typename Return, typename... Args>
    std::function<Return(Args...)> make_callback(analysis_context& c, Return (*callback)(analysis_context&, Args...))
    {
        return [&c, callback](Args... args) {
            return callback(c, std::forward<Args>(args)...); //
        };
    }

    template <typename Return, typename... Args>
    std::function<Return(Args...)> make_callback(analysis_context& c,
                                                 Return (*callback)(const analysis_context&, Args...))
    {
        return [&c, callback](Args... args) {
            return callback(c, std::forward<Args>(args)...); //
        };
    }

    void handle_suspicious_activity(const analysis_context& c, const std::string_view details)
    {
        const auto rip = c.win_emu->emu().read_instruction_pointer();
        c.win_emu->log.print(color::pink, "Suspicious: %.*s (0x%" PRIx64 ")\n", STR_VIEW_VA(details), rip);
    }

    void handle_instruction(analysis_context& c, const uint64_t address)
    {
        auto& win_emu = *c.win_emu;

        const auto is_main_exe = win_emu.mod_manager.executable->is_within(address);
        const auto is_previous_main_exe = win_emu.mod_manager.executable->is_within(c.win_emu->process.previous_ip);

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
            if (c.settings->modules.empty())
            {
                return false;
            }

            return (binary && c.settings->modules.contains(binary->name)) ||
                   (previous_binary && c.settings->modules.contains(previous_binary->name));
        };

        const auto is_interesting_call = is_previous_main_exe //
                                         || is_main_exe       //
                                         || is_in_interesting_module();

        if (!c.has_reached_main && c.settings->concise_logging && !c.settings->silent && is_main_exe)
        {
            c.has_reached_main = true;
            win_emu.log.disable_output(false);
        }

        if ((!c.settings->verbose_logging && !is_interesting_call) || !binary)
        {
            return;
        }

        const auto export_entry = binary->address_names.find(address);
        if (export_entry != binary->address_names.end() &&
            !c.settings->ignored_functions.contains(export_entry->second))
        {
            const auto rsp = win_emu.emu().read_stack_pointer();

            uint64_t return_address{};
            win_emu.emu().try_read_memory(rsp, &return_address, sizeof(return_address));

            const auto* mod_name = win_emu.mod_manager.find_name(return_address);

            win_emu.log.print(is_interesting_call ? color::yellow : color::dark_gray,
                              "Executing function: %s - %s (0x%" PRIx64 ") via (0x%" PRIx64 ") %s\n",
                              binary->name.c_str(), export_entry->second.c_str(), address, return_address, mod_name);
        }
        else if (address == binary->entry_point)
        {
            win_emu.log.print(is_interesting_call ? color::yellow : color::gray,
                              "Executing entry point: %s (0x%" PRIx64 ")\n", binary->name.c_str(), address);
        }
    }

    emulator_callbacks::continuation handle_syscall(const analysis_context& c, const uint32_t syscall_id,
                                                    const std::string_view syscall_name)
    {
        auto& win_emu = *c.win_emu;
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

    void handle_stdout(analysis_context& c, const std::string_view data)
    {
        if (c.settings->silent)
        {
            (void)fwrite(data.data(), 1, data.size(), stdout);
        }
        else if (c.settings->buffer_stdout)
        {
            c.output.append(data);
        }
        else
        {
            c.win_emu->log.info("%.*s%s", static_cast<int>(data.size()), data.data(), data.ends_with("\n") ? "" : "\n");
        }
    }
}

void register_analysis_callbacks(analysis_context& c)
{
    auto& cb = c.win_emu->callbacks;

    cb.on_stdout = make_callback(c, handle_stdout);
    cb.on_syscall = make_callback(c, handle_syscall);
    cb.on_instruction = make_callback(c, handle_instruction);
    cb.on_suspicious_activity = make_callback(c, handle_suspicious_activity);
}
