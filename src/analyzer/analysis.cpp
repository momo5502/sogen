#include "std_include.hpp"

#include "analysis.hpp"
#include "windows_emulator.hpp"
#include <utils/lazy_object.hpp>

#ifdef OS_EMSCRIPTEN
#include <event_handler.hpp>
#endif

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
        c.win_emu->log.print(color::pink, "Suspicious: %.*s at 0x%" PRIx64 " (via 0x%" PRIx64 ")\n",
                             STR_VIEW_VA(details), rip, c.win_emu->process.previous_ip);
    }

    void handle_generic_activity(const analysis_context& c, const std::string_view details)
    {
        c.win_emu->log.print(color::dark_gray, "%.*s\n", STR_VIEW_VA(details));
    }

    void handle_generic_access(const analysis_context& c, const std::string_view type, const std::u16string_view name)
    {
        c.win_emu->log.print(color::dark_gray, "--> %.*s: %s\n", STR_VIEW_VA(type), u16_to_u8(name).c_str()); //
    }

    void handle_memory_allocate(const analysis_context& c, const uint64_t address, const uint64_t length,
                                const memory_permission permission, const bool commit)
    {
        const auto* action = commit ? "Committed" : "Allocated";

        c.win_emu->log.print(is_executable(permission) ? color::gray : color::dark_gray,
                             "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n", action, address, address + length,
                             get_permission_string(permission).c_str());
    }

    void handle_memory_protect(const analysis_context& c, const uint64_t address, const uint64_t length,
                               const memory_permission permission)
    {
        c.win_emu->log.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n",
                             address, address + length, get_permission_string(permission).c_str());
    }

    void handle_memory_violate(const analysis_context& c, const uint64_t address, const uint64_t size,
                               const memory_operation operation, const memory_violation_type type)
    {
        const auto permission = get_permission_string(operation);
        const auto ip = c.win_emu->emu().read_instruction_pointer();
        const char* name = c.win_emu->mod_manager.find_name(ip);

        if (type == memory_violation_type::protection)
        {
            c.win_emu->log.print(color::gray,
                                 "Protection violation: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n",
                                 address, size, permission.c_str(), ip, name);
        }
        else if (type == memory_violation_type::unmapped)
        {
            c.win_emu->log.print(color::gray,
                                 "Mapping violation: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n", address,
                                 size, permission.c_str(), ip, name);
        }
    }

    void handle_ioctrl(const analysis_context& c, const io_device&, const std::u16string_view device_name,
                       const ULONG code)
    {
        c.win_emu->log.print(color::dark_gray, "--> %s: 0x%X\n", u16_to_u8(device_name).c_str(),
                             static_cast<uint32_t>(code));
    }

    void handle_thread_set_name(const analysis_context& c, const emulator_thread& t)
    {
        c.win_emu->log.print(color::blue, "Setting thread (%d) name: %s\n", t.id, u16_to_u8(t.name).c_str());
    }

    void handle_thread_switch(const analysis_context& c, const emulator_thread& current_thread,
                              const emulator_thread& new_thread)
    {
        c.win_emu->log.print(color::dark_gray, "Performing thread switch: %X -> %X\n", current_thread.id,
                             new_thread.id);
    }

    void handle_module_load(const analysis_context& c, const mapped_module& mod)
    {
        c.win_emu->log.log("Mapped %s at 0x%" PRIx64 "\n", mod.path.generic_string().c_str(), mod.image_base);
    }

    void handle_module_unload(const analysis_context& c, const mapped_module& mod)
    {
        c.win_emu->log.log("Unmapping %s (0x%" PRIx64 ")\n", mod.path.generic_string().c_str(), mod.image_base);
    }

    void handle_function_details(analysis_context& c, const std::string_view function)
    {
        if (function == "GetEnvironmentVariableA" || function == "ExpandEnvironmentStringsA")
        {
            const auto var_ptr = c.win_emu->emu().reg(x86_register::rcx);
            if (var_ptr)
            {
                const auto variable = read_string<char>(c.win_emu->memory, var_ptr);
                c.win_emu->log.print(color::dark_gray, "--> %s\n", variable.c_str());
            }
        }
    }

    void handle_instruction(analysis_context& c, const uint64_t address)
    {
        auto& win_emu = *c.win_emu;

#ifdef OS_EMSCRIPTEN
        if ((win_emu.get_executed_instructions() % 0x20000) == 0)
        {
            debugger::event_context ec{.win_emu = win_emu};
            debugger::handle_events(ec);
        }
#endif

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

            if (is_interesting_call)
            {
                handle_function_details(c, export_entry->second);
            }
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
    cb.on_ioctrl = make_callback(c, handle_ioctrl);

    cb.on_memory_protect = make_callback(c, handle_memory_protect);
    cb.on_memory_violate = make_callback(c, handle_memory_violate);
    cb.on_memory_allocate = make_callback(c, handle_memory_allocate);

    cb.on_module_load = make_callback(c, handle_module_load);
    cb.on_module_unload = make_callback(c, handle_module_unload);

    cb.on_thread_switch = make_callback(c, handle_thread_switch);
    cb.on_thread_set_name = make_callback(c, handle_thread_set_name);

    cb.on_instruction = make_callback(c, handle_instruction);
    cb.on_generic_access = make_callback(c, handle_generic_access);
    cb.on_generic_activity = make_callback(c, handle_generic_activity);
    cb.on_suspicious_activity = make_callback(c, handle_suspicious_activity);
}
