#include "std_include.hpp"

#include "analysis.hpp"
#include "analysis_reporter.hpp"
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

    bool is_int_resource(const uint64_t address)
    {
        return (address >> 0x10) == 0;
    }

    template <typename CharType = char>
    std::string read_arg_as_string(windows_emulator& win_emu, const size_t index)
    {
        const auto var_ptr = get_function_argument(win_emu.emu(), index);
        if (!var_ptr || is_int_resource(var_ptr))
        {
            return {};
        }

        try
        {
            const auto str = read_string<CharType>(win_emu.memory, var_ptr);
            if constexpr (std::is_same_v<CharType, char16_t>)
            {
                return u16_to_u8(str);
            }
            else
            {
                return str;
            }
        }
        catch (...)
        {
            return "[failed to read]";
        }
    }

    std::string read_module_name(windows_emulator& win_emu, const size_t index)
    {
        const auto var_ptr = get_function_argument(win_emu.emu(), index);
        if (!var_ptr)
        {
            return {};
        }

        const auto* module_name = win_emu.mod_manager.find_name(var_ptr);
        return module_name ? module_name : "<N/A>"s;
    }

    struct function_detail_result
    {
        std::vector<function_execution_detail> details{};
        bool stubbed_win_verify_trust{};
    };

    function_detail_result collect_function_details(const analysis_context& c, const std::string_view function)
    {
        function_detail_result result{};

        const auto push_detail = [&](std::string value, std::string label = {}) {
            if (!value.empty())
            {
                result.details.emplace_back(function_execution_detail{.label = std::move(label), .value = std::move(value)});
            }
        };

        if (function == "GetEnvironmentVariableA"      //
            || function == "ExpandEnvironmentStringsA" //
            || function == "LoadLibraryA")
        {
            push_detail(read_arg_as_string(*c.win_emu, 0));
        }
        else if (function == "LoadLibraryW")
        {
            push_detail(read_arg_as_string<char16_t>(*c.win_emu, 0));
        }
        else if (function == "MessageBoxA")
        {
            push_detail(read_arg_as_string(*c.win_emu, 2));
            push_detail(read_arg_as_string(*c.win_emu, 1));
        }
        else if (function == "MessageBoxW")
        {
            push_detail(read_arg_as_string<char16_t>(*c.win_emu, 2));
            push_detail(read_arg_as_string<char16_t>(*c.win_emu, 1));
        }
        else if (function == "GetProcAddress")
        {
            push_detail(read_module_name(*c.win_emu, 0));
            push_detail(read_arg_as_string(*c.win_emu, 1));
        }
        else if (function == "WinVerifyTrust")
        {
            auto& emu = c.win_emu->emu();
            emu.reg(x86_register::rip, emu.read_stack(0));
            emu.reg(x86_register::rsp, emu.reg(x86_register::rsp) + 8);
            emu.reg(x86_register::rax, 0);
            result.stubbed_win_verify_trust = true;
        }
        else if (function == "lstrcmp" || function == "lstrcmpi")
        {
            push_detail(read_arg_as_string(*c.win_emu, 0));
            push_detail(read_arg_as_string(*c.win_emu, 1));
        }

        return result;
    }

    void handle_suspicious_activity(const analysis_context& c, const std::string_view details)
    {
        std::string decoded_instruction{};
        const auto rip = c.win_emu->emu().read_instruction_pointer();

        if (details == "Illegal instruction")
        {
            decoded_instruction = get_instruction_string(c.d, c.win_emu->emu(), rip);
        }

        c.emit_observation(suspicious_activity_payload{
            .details = std::string(details),
            .decoded_instruction = std::move(decoded_instruction),
        });
    }

    void handle_debug_string(const analysis_context& c, const std::string_view details)
    {
        c.emit_observation(debug_string_payload{.details = std::string(details)});
    }

    void handle_generic_activity(const analysis_context& c, const std::string_view details)
    {
        c.emit_observation(generic_activity_payload{.details = std::string(details)});
    }

    void handle_generic_access(const analysis_context& c, const std::string_view type, const std::u16string_view name)
    {
        c.emit_observation(generic_access_payload{.type = std::string(type), .name = u16_to_u8(name)});
    }

    void handle_memory_allocate(const analysis_context& c, const uint64_t address, const uint64_t length,
                                const memory_permission permission, const bool commit)
    {
        c.emit_observation(memory_allocate_payload{
            .address = address,
            .length = length,
            .permissions = get_permission_string(permission),
            .commit = commit,
        });
    }

    void handle_memory_protect(const analysis_context& c, const uint64_t address, const uint64_t length, const memory_permission permission)
    {
        c.emit_observation(memory_protect_payload{
            .address = address,
            .length = length,
            .permissions = get_permission_string(permission),
        });
    }

    void handle_memory_violate(const analysis_context& c, const uint64_t address, const uint64_t size, const memory_operation operation,
                               const memory_violation_type type)
    {
        c.emit_observation(memory_violation_payload{
            .address = address,
            .size = size,
            .operation = get_permission_string(operation),
            .violation_type = type == memory_violation_type::protection ? "protection"s : "unmapped"s,
        });

        if (type == memory_violation_type::unmapped)
        {
            if (c.mapping_violation.first == address)
            {
                if (++c.mapping_violation.second > 5)
                {
                    throw std::runtime_error("Too many identical violations. Aborting...");
                }
            }
            else
            {
                c.mapping_violation.first = address;
                c.mapping_violation.second = 1;
            }
        }
    }

    void handle_ioctrl(const analysis_context& c, const io_device&, const std::u16string_view device_name, const ULONG code)
    {
        c.emit_observation(io_control_payload{.device_name = u16_to_u8(device_name), .code = static_cast<uint32_t>(code)});
    }

    void handle_thread_create(const analysis_context& c, handle, emulator_thread& t)
    {
        std::vector<std::string> flags{};

        if (t.create_flags & THREAD_CREATE_FLAGS_CREATE_SUSPENDED)
            flags.emplace_back("suspended");
        if (t.create_flags & THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH)
            flags.emplace_back("skip thread attach");
        if (t.create_flags & THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
            flags.emplace_back("hide from debugger");
        if (t.create_flags & THREAD_CREATE_FLAGS_LOADER_WORKER)
            flags.emplace_back("loader worker");
        if (t.create_flags & THREAD_CREATE_FLAGS_SKIP_LOADER_INIT)
            flags.emplace_back("skip loader init");
        if (t.create_flags & THREAD_CREATE_FLAGS_BYPASS_PROCESS_FREEZE)
            flags.emplace_back("bypass process freeze");

        c.emit_observation(thread_create_payload{
            .created_thread_id = t.id,
            .start_address = t.start_address,
            .argument = t.argument,
            .flags = std::move(flags),
        });
    }

    void handle_thread_terminated(const analysis_context& c, handle, emulator_thread& t)
    {
        c.emit_observation(thread_terminated_payload{.terminated_thread_id = t.id});
    }

    void handle_thread_set_name(const analysis_context& c, const emulator_thread& t)
    {
        c.emit_observation(thread_set_name_payload{.renamed_thread_id = t.id, .name = u16_to_u8(t.name)});
    }

    void handle_thread_switch(const analysis_context& c, const emulator_thread& current_thread, const emulator_thread& new_thread)
    {
        c.emit_observation(thread_switch_payload{.previous_thread_id = current_thread.id, .next_thread_id = new_thread.id});
    }

    void handle_module_load(const analysis_context& c, const mapped_module& mod)
    {
        c.emit_observation(module_load_payload{.path = mod.module_path.string(), .image_base = mod.image_base});
    }

    void handle_module_unload(const analysis_context& c, const mapped_module& mod)
    {
        c.emit_observation(module_unload_payload{.path = mod.module_path.string(), .image_base = mod.image_base});
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

            c.emit_observation(import_read_payload{
                .resolved_address = a.address,
                .import_name = a.import_name,
                .import_module = a.import_module,
            });

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

    void log_first_section_execution(analysis_context& c, mapped_module* binary, const uint64_t address)
    {
        if (!binary)
        {
            return;
        }

        for (auto& section : binary->sections)
        {
            if (!is_within_start_and_length(address, section.region.start, section.region.length))
            {
                continue;
            }

            if (!section.first_execute.has_value())
            {
                section.first_execute = address;
                c.emit_observation(section_first_execute_payload{
                    .module_name = binary->name,
                    .section_name = section.name,
                    .file_address = *section.first_execute - binary->image_base + binary->image_base_file,
                });
            }

            break;
        }
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

        if (c.settings->log_first_section_execution)
        {
            log_first_section_execution(c, binary, address);
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
                auto details = collect_function_details(c, export_entry->second);
                c.emit_observation(function_execution_payload{
                    .function_name = export_entry->second,
                    .interesting = is_interesting_call,
                    .details = std::move(details.details),
                    .stubbed_win_verify_trust = details.stubbed_win_verify_trust,
                });
            }
        }
        else if (address == binary->entry_point)
        {
            c.emit_observation(entry_point_execution_payload{.interesting = is_interesting_call});
        }
        else if (is_previous_main_exe && binary != previous_binary && !is_return(c.d, c.win_emu->emu(), previous_ip))
        {
            auto nearest_entry = binary->address_names.upper_bound(address);
            if (nearest_entry == binary->address_names.begin())
            {
                return;
            }

            --nearest_entry;
            c.emit_observation(foreign_code_transition_payload{
                .function_name = nearest_entry->second,
                .function_offset = address - nearest_entry->first,
                .interesting = is_interesting_call,
            });
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

        c.emit_observation(rdtsc_payload{});
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

        c.emit_observation(rdtscp_payload{});
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
        const auto is_valid_32_bit_module = utils::make_lazy([&] {
            return mod                                                              //
                   && win_emu.process.is_wow64_process                              //
                   && (mod->name == "wow64cpu.dll" || mod->name == "wow64win.dll"); //
        });

        if (is_sus_module && !is_valid_32_bit_module)
        {
            c.emit_observation(syscall_payload{
                .classification = syscall_classification::inline_syscall,
                .syscall_id = syscall_id,
                .syscall_name = std::string(syscall_name),
            });
        }
        else if (!previous_ip || mod->contains(previous_ip))
        {
            if (!c.settings->skip_syscalls)
            {
                const auto rsp = emu.read_stack_pointer();

                uint64_t return_address{};
                emu.try_read_memory(rsp, &return_address, sizeof(return_address));

                const auto* caller_mod_name = win_emu.mod_manager.find_name(return_address);

                c.emit_observation(syscall_payload{
                    .classification = syscall_classification::regular,
                    .syscall_id = syscall_id,
                    .syscall_name = std::string(syscall_name),
                    .caller_rip = return_address,
                    .caller_module = caller_mod_name ? std::optional<std::string>{caller_mod_name} : std::nullopt,
                });
            }
        }
        else
        {
            const auto* previous_mod = win_emu.mod_manager.find_by_address(previous_ip);

            c.emit_observation(syscall_payload{
                .classification = syscall_classification::crafted_out_of_line,
                .syscall_id = syscall_id,
                .syscall_name = std::string(syscall_name),
                .caller_rip = previous_ip,
                .caller_module = previous_mod ? std::optional<std::string>{previous_mod->name} : std::nullopt,
            });
        }

        return instruction_hook_continuation::run_instruction;
    }

    void handle_stdout(analysis_context& c, const std::string_view data)
    {
        c.emit_observation(stdout_chunk_payload{.data = std::string(data)});

        if (c.settings->buffer_stdout && !c.settings->silent)
        {
            c.output.append(data);
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

            c.emit_observation(import_write_payload{
                .size = size,
                .value = int_value,
                .import_name = sym->second.name,
                .import_module = import_module,
            });
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

event_header analysis_context::make_event_header() const
{
    return {
        .sequence = this->next_event_sequence++,
        .instruction_count = this->win_emu ? this->win_emu->get_executed_instructions() : 0,
    };
}

execution_context analysis_context::make_execution_context() const
{
    auto& emu = this->win_emu->emu();
    const auto rip = emu.read_instruction_pointer();
    const auto* rip_module = this->win_emu->mod_manager.find_name(rip);

    execution_context context{
        .thread_id = 0,
        .rip = rip,
        .rip_module = rip_module ? rip_module : "<N/A>",
    };

    try
    {
        const auto& thread = this->win_emu->current_thread();
        const auto previous_ip = thread.previous_ip;
        const auto* previous_module = previous_ip ? this->win_emu->mod_manager.find_name(previous_ip) : nullptr;
        context.thread_id = thread.id;
        context.previous_ip = previous_ip ? std::optional<uint64_t>{previous_ip} : std::nullopt;
        context.previous_ip_module = previous_module ? std::optional<std::string>{previous_module} : std::nullopt;
    }
    catch (...)
    {
        // Some early lifecycle events fire before a thread is active.
    }

    return context;
}

void analysis_context::emit_event(analysis_event event) const
{
    for (auto* reporter : this->reporters)
    {
        reporter->report(event);
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
    cb.on_debug_string.add(make_callback(c, handle_debug_string));
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
