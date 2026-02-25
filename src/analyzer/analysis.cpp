#include "std_include.hpp"

#include "analysis.hpp"
#include "disassembler.hpp"
#include "windows_emulator.hpp"
#include <utils/lazy_object.hpp>

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
#include <event_handler.hpp>
#endif

#define STR_VIEW_VA(str) static_cast<int>((str).size()), (str).data()

namespace
{
    constexpr size_t MAX_INSTRUCTION_BYTES = 15;

    template <typename Return, typename... Args>
    std::function<Return(Args...)> make_callback(analysis_context& c, Return (*callback)(analysis_context&, Args...))
    {
        return [&c, callback](Args... args) {
            return callback(c, std::forward<Args>(args)...); //
        };
    }

    template <typename Return, typename... Args>
    std::function<Return(Args...)> make_callback(analysis_context& c, Return (*callback)(const analysis_context&, Args...))
    {
        return [&c, callback](Args... args) {
            return callback(c, std::forward<Args>(args)...); //
        };
    }

    std::string get_instruction_string(const disassembler& d, const emulator& emu, const uint64_t address)
    {
        std::array<uint8_t, MAX_INSTRUCTION_BYTES> instruction_bytes{};
        const auto result = emu.try_read_memory(address, instruction_bytes.data(), instruction_bytes.size());
        if (!result)
        {
            return {};
        }

        uint16_t reg_cs = 0;
        auto& emu_ref = const_cast<emulator&>(emu);
        emu_ref.read_raw_register(static_cast<int>(x86_register::cs), &reg_cs, sizeof(reg_cs));
        const auto instructions = d.disassemble(emu_ref, reg_cs, instruction_bytes, 1);
        if (instructions.empty())
        {
            return {};
        }

        const auto& inst = instructions[0];
        return std::string(inst.mnemonic) + (strlen(inst.op_str) ? " "s + inst.op_str : "");
    }

    void handle_suspicious_activity(const analysis_context& c, const std::string_view details)
    {
        std::string addition{};
        const auto rip = c.win_emu->emu().read_instruction_pointer();

        // TODO: Pass enum?
        if (details == "Illegal instruction")
        {
            const auto inst = get_instruction_string(c.d, c.win_emu->emu(), rip);
            if (!inst.empty())
            {
                addition = " (" + inst + ")";
            }
        }

        const auto previous_ip = c.win_emu->current_thread().previous_ip;
        c.win_emu->log.print(color::pink, "Suspicious: %.*s%.*s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n", STR_VIEW_VA(details),
                             STR_VIEW_VA(addition), rip, previous_ip, c.win_emu->mod_manager.find_name(previous_ip));
    }

    void handle_debug_string(const analysis_context& c, const std::string_view details)
    {
        c.win_emu->log.info("--> Debug string: %.*s\n", STR_VIEW_VA(details));
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
        const auto* action = commit ? "Committed" : "Allocating";

        c.win_emu->log.print(is_executable(permission) ? color::gray : color::dark_gray, "--> %s 0x%" PRIx64 " - 0x%" PRIx64 " (%s)\n",
                             action, address, address + length, get_permission_string(permission).c_str());
    }

    void handle_memory_protect(const analysis_context& c, const uint64_t address, const uint64_t length, const memory_permission permission)
    {
        c.win_emu->log.print(color::dark_gray, "--> Changing protection at 0x%" PRIx64 "-0x%" PRIx64 " to %s\n", address, address + length,
                             get_permission_string(permission).c_str());
    }

    void handle_memory_violate(const analysis_context& c, const uint64_t address, const uint64_t size, const memory_operation operation,
                               const memory_violation_type type)
    {
        const auto permission = get_permission_string(operation);
        const auto ip = c.win_emu->emu().read_instruction_pointer();
        const char* name = c.win_emu->mod_manager.find_name(ip);

        if (type == memory_violation_type::protection)
        {
            c.win_emu->log.print(color::gray, "Protection violation: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n", address,
                                 size, permission.c_str(), ip, name);
        }
        else if (type == memory_violation_type::unmapped)
        {
            c.win_emu->log.print(color::gray, "Mapping violation: 0x%" PRIx64 " (%" PRIx64 ") - %s at 0x%" PRIx64 " (%s)\n", address, size,
                                 permission.c_str(), ip, name);
        }
    }

    void handle_ioctrl(const analysis_context& c, const io_device&, const std::u16string_view device_name, const ULONG code)
    {
        c.win_emu->log.print(color::dark_gray, "--> %s: 0x%X\n", u16_to_u8(device_name).c_str(), static_cast<uint32_t>(code));
    }

    void handle_thread_create(const analysis_context& c, handle, emulator_thread& t)
    {
        std::vector<std::string> flags{};

        if (t.create_flags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED)
        {
            flags.emplace_back("suspended");
        }
        if (t.create_flags & THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH)
        {
            flags.emplace_back("skip thread attach");
        }
        if (t.create_flags & THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
        {
            flags.emplace_back("hide from debugger");
        }
        if (t.create_flags & THREAD_CREATE_FLAGS_LOADER_WORKER)
        {
            flags.emplace_back("loader worker");
        }
        if (t.create_flags & THREAD_CREATE_FLAGS_SKIP_LOADER_INIT)
        {
            flags.emplace_back("skip loader init");
        }
        if (t.create_flags & THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE)
        {
            flags.emplace_back("bypass process freeze");
        }

        std::string flags_string{};
        for (const auto& flag : flags)
        {
            flags_string += ", ";
            flags_string += flag;
        }

        c.win_emu->log.print(color::gray, "Thread created: tid %d, start address 0x%" PRIx64 " (param 0x%" PRIx64 ")%s\n", t.id,
                             t.start_address, t.argument, flags_string.c_str());
    }

    void handle_thread_terminated(const analysis_context& c, handle, emulator_thread& t)
    {
        c.win_emu->log.print(color::gray, "Thread terminated: tid %d\n", t.id);
    }

    void handle_thread_set_name(const analysis_context& c, const emulator_thread& t)
    {
        c.win_emu->log.print(color::blue, "Setting thread (%u) name: %s\n", t.id, u16_to_u8(t.name).c_str());
    }

    void handle_thread_switch(const analysis_context& c, const emulator_thread& current_thread, const emulator_thread& new_thread)
    {
        c.win_emu->log.print(color::dark_gray, "Performing thread switch: %X -> %X\n", current_thread.id, new_thread.id);
    }

    void handle_module_load(const analysis_context& c, const mapped_module& mod)
    {
        c.win_emu->log.log("Mapped %s at 0x%" PRIx64 "\n", mod.path.generic_string().c_str(), mod.image_base);
    }

    void handle_module_unload(const analysis_context& c, const mapped_module& mod)
    {
        c.win_emu->log.log("Unmapping %s (0x%" PRIx64 ")\n", mod.path.generic_string().c_str(), mod.image_base);
    }

    void print_string(logger& log, const std::string_view str)
    {
        log.print(color::dark_gray, "--> %.*s\n", STR_VIEW_VA(str));
    }

    void print_string(logger& log, const std::u16string_view str)
    {
        print_string(log, u16_to_u8(str));
    }

    bool is_int_resource(const uint64_t address)
    {
        return (address >> 0x10) == 0;
    }

    template <typename CharType = char>
    void print_arg_as_string(windows_emulator& win_emu, const size_t index)
    {
        const auto var_ptr = get_function_argument(win_emu.emu(), index);
        if (var_ptr && !is_int_resource(var_ptr))
        {
            try
            {
                const auto str = read_string<CharType>(win_emu.memory, var_ptr);
                print_string(win_emu.log, str);
            }
            catch (...)
            {
                print_string(win_emu.log, "[failed to read]");
            }
        }
    }

    void print_module_name(windows_emulator& win_emu, const size_t index)
    {
        const auto var_ptr = get_function_argument(win_emu.emu(), index);
        if (var_ptr)
        {
            const auto* module_name = win_emu.mod_manager.find_name(var_ptr);
            print_string(win_emu.log, module_name);
        }
    }

    void handle_function_details(const analysis_context& c, const std::string_view function)
    {
        if (function == "GetEnvironmentVariableA"      //
            || function == "ExpandEnvironmentStringsA" //
            || function == "LoadLibraryA")
        {
            print_arg_as_string(*c.win_emu, 0);
        }
        else if (function == "LoadLibraryW")
        {
            print_arg_as_string<char16_t>(*c.win_emu, 0);
        }
        else if (function == "MessageBoxA")
        {
            print_arg_as_string(*c.win_emu, 2);
            print_arg_as_string(*c.win_emu, 1);
        }
        else if (function == "MessageBoxW")
        {
            print_arg_as_string<char16_t>(*c.win_emu, 2);
            print_arg_as_string<char16_t>(*c.win_emu, 1);
        }
        else if (function == "GetProcAddress")
        {
            print_module_name(*c.win_emu, 0);
            print_arg_as_string(*c.win_emu, 1);
        }
        else if (function == "WinVerifyTrust")
        {
            auto& emu = c.win_emu->emu();
            emu.reg(x86_register::rip, emu.read_stack(0));
            emu.reg(x86_register::rsp, emu.reg(x86_register::rsp) + 8);
            emu.reg(x86_register::rax, 0);
        }
        else if (function == "lstrcmp" || function == "lstrcmpi")
        {
            print_arg_as_string(*c.win_emu, 0);
            print_arg_as_string(*c.win_emu, 1);
        }
    }

    bool is_thread_alive(const analysis_context& c, const uint32_t thread_id)
    {
        for (const auto& t : c.win_emu->process.threads | std::views::values)
        {
            if (t.id == thread_id)
            {
                return true;
            }
        }

        return false;
    }

    void update_import_access(analysis_context& c, const uint64_t address)
    {
        if (c.accessed_imports.empty())
        {
            return;
        }

        const auto& t = c.win_emu->current_thread();
        for (auto entry = c.accessed_imports.begin(); entry != c.accessed_imports.end();)
        {
            auto& a = *entry;
            const auto is_same_thread = t.id == a.thread_id;

            if (is_same_thread && address == a.address)
            {
                entry = c.accessed_imports.erase(entry);
                continue;
            }

            constexpr auto inst_delay = 100u;
            const auto execution_delay_reached = is_same_thread && a.access_inst_count + inst_delay <= t.executed_instructions;

            if (!execution_delay_reached && is_thread_alive(c, a.thread_id))
            {
                ++entry;
                continue;
            }

            c.win_emu->log.print(color::green, "Import read access: %s (%s) at 0x%" PRIx64 " (%s)\n", a.import_name.c_str(),
                                 a.import_module.c_str(), a.access_rip, a.accessor_module.c_str());

            entry = c.accessed_imports.erase(entry);
        }
    }

    bool is_return(const disassembler& d, const emulator& emu, const uint64_t address)
    {
        std::array<uint8_t, MAX_INSTRUCTION_BYTES> instruction_bytes{};
        const auto result = emu.try_read_memory(address, instruction_bytes.data(), instruction_bytes.size());
        if (!result)
        {
            return false;
        }

        uint16_t reg_cs = 0;
        auto& emu_ref = const_cast<emulator&>(emu);
        emu_ref.read_raw_register(static_cast<int>(x86_register::cs), &reg_cs, sizeof(reg_cs));
        const auto instructions = d.disassemble(emu_ref, reg_cs, instruction_bytes, 1);
        if (instructions.empty())
        {
            return false;
        }

        const auto handle = d.resolve_handle(emu_ref, reg_cs);
        return cs_insn_group(handle, instructions.data(), CS_GRP_RET);
    }

    void record_instruction(analysis_context& c, const uint64_t address)
    {
        auto& emu = c.win_emu->emu();
        std::array<uint8_t, MAX_INSTRUCTION_BYTES> instruction_bytes{};
        const auto result = emu.try_read_memory(address, instruction_bytes.data(), instruction_bytes.size());
        if (!result)
        {
            return;
        }

        const auto reg_cs = emu.reg<uint16_t>(x86_register::cs);
        disassembler disasm{};
        const auto instructions = disasm.disassemble(emu, reg_cs, instruction_bytes, 1);
        if (instructions.empty())
        {
            return;
        }

        ++c.instructions[instructions[0].id];
    }

    void handle_instruction(analysis_context& c, const uint64_t address)
    {
        auto& win_emu = *c.win_emu;
        update_import_access(c, address);

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
        if ((win_emu.get_executed_instructions() % 0x20000) == 0)
        {
            debugger::event_context ec{.win_emu = win_emu};
            debugger::handle_events(ec);
        }
#endif

        const auto& current_thread = c.win_emu->current_thread();
        const auto previous_ip = current_thread.previous_ip;
        [[maybe_unused]] const auto current_ip = current_thread.current_ip;
        const auto is_main_exe = win_emu.mod_manager.executable->contains(address);
        const auto is_previous_main_exe = win_emu.mod_manager.executable->contains(previous_ip);

        const auto binary = utils::make_lazy([&] {
            if (is_main_exe)
            {
                return win_emu.mod_manager.executable;
            }

            return win_emu.mod_manager.find_by_address(address); //
        });

        if (binary)
        {
            for (auto& section : binary->sections)
            {
                if (is_within_start_and_length(address, section.region.start, section.region.length) && section.first_execute == UINT64_MAX)
                {
                    section.first_execute = address;
                    win_emu.log.print(color::green, "Section %s (%s) first execute at 0x%" PRIx64 " 0x%" PRIx64 " (tid: %" PRIx32 ")\n",
                                      binary->name.c_str(), section.name.c_str(), section.first_execute,
                                      section.first_execute - binary->image_base + binary->image_base_file, current_thread.id);
                }
            }
        }

        const auto previous_binary = utils::make_lazy([&] {
            if (is_previous_main_exe)
            {
                return win_emu.mod_manager.executable;
            }

            return win_emu.mod_manager.find_by_address(previous_ip); //
        });

        const auto is_current_binary_interesting = utils::make_lazy([&] {
            return is_main_exe || (binary && c.settings->modules.contains(binary->name)); //
        });

        const auto is_in_interesting_module = [&] {
            if (c.settings->modules.empty())
            {
                return false;
            }

            return is_current_binary_interesting || (previous_binary && c.settings->modules.contains(previous_binary->name));
        };

        if (c.settings->instruction_summary && (is_current_binary_interesting || !binary))
        {
            record_instruction(c, address);
        }

        const auto is_interesting_call = is_previous_main_exe                                              //
                                         || (!previous_binary && current_thread.executed_instructions > 1) //
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
        if (export_entry != binary->address_names.end())
        {
            if (!c.settings->ignored_functions.contains(export_entry->second))
            {
                win_emu.log.print(is_interesting_call ? color::yellow : color::dark_gray,
                                  "Executing function: %s (%s) (0x%" PRIx64 ") via (0x%" PRIx64 ") %s\n", export_entry->second.c_str(),
                                  binary->name.c_str(), address, previous_ip, previous_binary ? previous_binary->name.c_str() : "<N/A>");

                if (is_interesting_call)
                {
                    handle_function_details(c, export_entry->second);
                }
            }
        }
        else if (address == binary->entry_point)
        {
            win_emu.log.print(is_interesting_call ? color::yellow : color::gray, "Executing entry point: %s (0x%" PRIx64 ")\n",
                              binary->name.c_str(), address);
        }
        else if (is_previous_main_exe && binary != previous_binary && !is_return(c.d, c.win_emu->emu(), previous_ip))
        {
            auto nearest_entry = binary->address_names.upper_bound(address);
            if (nearest_entry == binary->address_names.begin())
            {
                return;
            }

            --nearest_entry;

            win_emu.log.print(is_interesting_call ? color::yellow : color::dark_gray,
                              "Transition to foreign code: %s+0x%" PRIx64 " (%s) (0x%" PRIx64 ") via (0x%" PRIx64 ") %s\n",
                              nearest_entry->second.c_str(), address - nearest_entry->first, binary->name.c_str(), address, previous_ip,
                              previous_binary ? previous_binary->name.c_str() : "<N/A>");
        }
    }

    void handle_rdtsc(analysis_context& c)
    {
        auto& win_emu = *c.win_emu;
        auto& emu = win_emu.emu();

        const auto rip = emu.read_instruction_pointer();
        const auto mod = get_module_if_interesting(win_emu.mod_manager, c.settings->modules, rip);

        if (!mod.has_value() || (c.settings->concise_logging && !c.rdtsc_cache.insert(rip).second))
        {
            return;
        }

        win_emu.log.print(color::blue, "Executing RDTSC instruction at 0x%" PRIx64 " (%s)\n", rip, (*mod) ? (*mod)->name.c_str() : "<N/A>");
    }

    void handle_rdtscp(analysis_context& c)
    {
        auto& win_emu = *c.win_emu;
        auto& emu = win_emu.emu();

        const auto rip = emu.read_instruction_pointer();
        const auto mod = get_module_if_interesting(win_emu.mod_manager, c.settings->modules, rip);

        if (!mod.has_value() || (c.settings->concise_logging && !c.rdtscp_cache.insert(rip).second))
        {
            return;
        }

        win_emu.log.print(color::blue, "Executing RDTSCP instruction at 0x%" PRIx64 " (%s)\n", rip,
                          (*mod) ? (*mod)->name.c_str() : "<N/A>");
    }

    emulator_callbacks::continuation handle_syscall(const analysis_context& c, const uint32_t syscall_id,
                                                    const std::string_view syscall_name)
    {
        auto& win_emu = *c.win_emu;
        auto& emu = win_emu.emu();

        const auto address = emu.read_instruction_pointer();
        const auto* mod = win_emu.mod_manager.find_by_address(address);
        const auto is_sus_module = mod != win_emu.mod_manager.ntdll && mod != win_emu.mod_manager.win32u;
        const auto previous_ip = win_emu.current_thread().previous_ip;

        if (is_sus_module)
        {
            win_emu.log.print(color::blue, "Executing inline syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s)\n", STR_VIEW_VA(syscall_name),
                              syscall_id, address, mod ? mod->name.c_str() : "<N/A>");
        }
        else if (mod->contains(previous_ip))
        {
            if (!c.settings->skip_syscalls)
            {
                const auto rsp = emu.read_stack_pointer();

                uint64_t return_address{};
                emu.try_read_memory(rsp, &return_address, sizeof(return_address));

                const auto* caller_mod_name = win_emu.mod_manager.find_name(return_address);

                win_emu.log.print(color::dark_gray, "Executing syscall: %.*s (0x%X) at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n",
                                  STR_VIEW_VA(syscall_name), syscall_id, address, return_address, caller_mod_name);
            }
        }
        else
        {
            const auto* previous_mod = win_emu.mod_manager.find_by_address(previous_ip);

            win_emu.log.print(color::blue, "Crafted out-of-line syscall: %.*s (0x%X) at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n",
                              STR_VIEW_VA(syscall_name), syscall_id, address, mod ? mod->name.c_str() : "<N/A>", previous_ip,
                              previous_mod ? previous_mod->name.c_str() : "<N/A>");
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

    void watch_import_table(analysis_context& c)
    {
        c.win_emu->setup_process_if_necessary();

        const auto& import_list = c.win_emu->mod_manager.executable->imports;
        if (import_list.empty())
        {
            return;
        }

        auto min = std::numeric_limits<uint64_t>::max();
        auto max = std::numeric_limits<uint64_t>::min();

        for (const auto& import_thunk : import_list | std::views::keys)
        {
            min = std::min(import_thunk, min);
            max = std::max(import_thunk, max);
        }

        c.win_emu->emu().hook_memory_write(min, max - min, [&c](const uint64_t address, const void* value, size_t size) {
            const auto rip = c.win_emu->emu().read_instruction_pointer();
            const auto& watched_module = *c.win_emu->mod_manager.executable;

            const auto sym = watched_module.imports.find(address);
            if (sym == watched_module.imports.end())
            {
                // TODO: Print unaligned write accesses?
                return;
            }

            uint64_t int_value{};
            memcpy(&int_value, value, std::min(size, sizeof(int_value)));

            const auto import_module = watched_module.imported_modules.at(sym->second.module_index);

            c.win_emu->log.print(color::blue, "Import write access: %zd bytes with value 0x%" PRIX64 " to %s (%s) at 0x%" PRIx64 " (%s)\n",
                                 size, int_value, sym->second.name.c_str(), import_module.c_str(), rip,
                                 c.win_emu->mod_manager.find_name(rip));
        });

        c.win_emu->emu().hook_memory_read(min, max - min, [&c](const uint64_t address, const void*, size_t) {
            const auto rip = c.win_emu->emu().read_instruction_pointer();
            const auto& watched_module = *c.win_emu->mod_manager.executable;
            const auto accessor_module = get_module_if_interesting(c.win_emu->mod_manager, c.settings->modules, rip);

            if (!accessor_module.has_value())
            {
                return;
            }

            const auto sym = watched_module.imports.find(address);
            if (sym == watched_module.imports.end())
            {
                return;
            }

            accessed_import access{};

            access.address = c.win_emu->emu().read_memory<uint64_t>(address);

            access.access_rip = rip;
            access.accessor_module = accessor_module.value() ? (*accessor_module)->name : "<N/A>";

            access.import_name = sym->second.name;
            access.import_module = watched_module.imported_modules.at(sym->second.module_index);

            const auto& t = c.win_emu->current_thread();
            access.thread_id = t.id;
            access.access_inst_count = t.executed_instructions;

            c.accessed_imports.push_back(std::move(access));
        });
    }
}

void register_analysis_callbacks(analysis_context& c)
{
    auto& cb = c.win_emu->callbacks;

    cb.on_stdout = make_callback(c, handle_stdout);
    cb.on_syscall = make_callback(c, handle_syscall);
    cb.on_rdtsc = make_callback(c, handle_rdtsc);
    cb.on_rdtscp = make_callback(c, handle_rdtscp);
    cb.on_ioctrl = make_callback(c, handle_ioctrl);

    cb.on_memory_protect = make_callback(c, handle_memory_protect);
    cb.on_memory_violate = make_callback(c, handle_memory_violate);
    cb.on_memory_allocate = make_callback(c, handle_memory_allocate);

    (void)cb.on_module_load.add(make_callback(c, handle_module_load));
    (void)cb.on_module_unload.add(make_callback(c, handle_module_unload));

    cb.on_thread_create = make_callback(c, handle_thread_create);
    cb.on_thread_terminated = make_callback(c, handle_thread_terminated);
    cb.on_thread_switch = make_callback(c, handle_thread_switch);
    cb.on_thread_set_name = make_callback(c, handle_thread_set_name);

    cb.on_instruction = make_callback(c, handle_instruction);
    cb.on_debug_string = make_callback(c, handle_debug_string);
    cb.on_generic_access = make_callback(c, handle_generic_access);
    cb.on_generic_activity = make_callback(c, handle_generic_activity);
    cb.on_suspicious_activity = make_callback(c, handle_suspicious_activity);

    watch_import_table(c);
}

std::optional<mapped_module*> get_module_if_interesting(module_manager& manager, const string_set& modules, const uint64_t address)
{
    if (manager.executable->contains(address))
    {
        return manager.executable;
    }

    auto* mod = manager.find_by_address(address);
    if (!mod)
    {
        // Not being part of any module is interesting
        return nullptr;
    }

    if (modules.contains(mod->name))
    {
        return mod;
    }

    return std::nullopt;
}
