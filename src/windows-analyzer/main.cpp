#include "std_include.hpp"

#include <CLI/CLI.hpp>

#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <win_x86_64_gdb_stub_handler.hpp>
#include <minidump_loader.hpp>
#include <scoped_hook.hpp>

#include "object_watching.hpp"
#include "snapshot.hpp"
#include "analysis.hpp"
#include "analysis_reporter.hpp"
#include "jsonl_reporter.hpp"
#include "tenet_tracer.hpp"

#include <utils/finally.hpp>
#include <utils/interupt_handler.hpp>

#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
#include <event_handler.hpp>
#endif

#ifndef _WIN32
#include <csignal>
#endif

namespace sogen
{
    namespace
    {
        std::filesystem::path get_current_binary_dir()
        {
#ifdef _WIN32
            std::array<wchar_t, MAX_PATH> buffer{};

            const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length == buffer.size())
            {
                throw std::runtime_error("Resolving module file name failed");
            }

            return std::filesystem::path(buffer.data()).parent_path();
#else
            return "./";
#endif
        }

        struct analysis_options : analysis_settings
        {
            mutable bool use_gdb{false};
            std::string gdb_host{"127.0.0.1"};
            uint16_t gdb_port{28960};
            bool log_executable_access{false};
            bool log_foreign_module_access{false};
            bool tenet_trace{false};
            bool prepend_call_count{false};
#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
            bool pause_before_start{false};
#endif
            std::optional<uint64_t> break_call{};
            std::filesystem::path dump{};
            std::filesystem::path minidump_path{};
            std::filesystem::path report_path{};
            std::string report_format{"jsonl"};
            std::string whp_execution_hook_mode{"auto"};
            std::optional<backend_type> backend{};
            bool disable_instruction_precision{false};
            uint32_t vcpu_count{1};
            std::filesystem::path registry_path{get_current_binary_dir() / "registry"};
            std::filesystem::path emulation_root{};
            std::unordered_map<windows_path, std::filesystem::path> path_mappings{};
            utils::unordered_insensitive_u16string_map<std::u16string> environment{};
        };

        void split_and_insert(std::set<std::string, std::less<>>& container, const std::string_view str, const char splitter = ',')
        {
            size_t current_start = 0;
            for (size_t i = 0; i < str.size(); ++i)
            {
                const auto value = str[i];
                if (value != splitter)
                {
                    continue;
                }

                if (current_start < i)
                {
                    container.emplace(str.substr(current_start, i - current_start));
                }

                current_start = i + 1;
            }

            if (current_start < str.size())
            {
                container.emplace(str.substr(current_start));
            }
        }

        struct analysis_state
        {
            analysis_context& context_;
            windows_emulator& win_emu_;
            scoped_hook env_data_hook_;
            scoped_hook env_ptr_hook_;
            scoped_hook params_hook_;
            scoped_hook ldr_hook_;
            std::map<std::string, uint64_t> env_module_cache_{};
            std::shared_ptr<object_watching_state> params_state_ = std::make_shared<object_watching_state>();
            std::shared_ptr<object_watching_state> ldr_state_ = std::make_shared<object_watching_state>();
            std::set<std::string, std::less<>> modules_;
            bool verbose_;
            bool concise_;

            analysis_state(analysis_context& context, std::set<std::string, std::less<>> modules, const bool verbose, const bool concise)
                : context_(context),
                  win_emu_(*context.win_emu),
                  env_data_hook_(context.win_emu->emu()),
                  env_ptr_hook_(context.win_emu->emu()),
                  params_hook_(context.win_emu->emu()),
                  ldr_hook_(context.win_emu->emu()),
                  modules_(std::move(modules)),
                  verbose_(verbose),
                  concise_(concise)
            {
            }
        };

        emulator_object<RTL_USER_PROCESS_PARAMETERS64> get_process_params(windows_emulator& win_emu)
        {
            const auto peb = win_emu.process.peb64.read();
            return {win_emu.emu(), peb.ProcessParameters};
        }

        uint64_t get_environment_ptr(windows_emulator& win_emu)
        {
            const auto process_params = get_process_params(win_emu);
            return process_params.read().Environment;
        }

        size_t get_environment_size(const x86_64_emulator& emu, const uint64_t env)
        {
            std::array<uint8_t, 4> data{};
            std::array<uint8_t, 4> empty{};

            for (size_t i = 0; i < 0x100000; ++i)
            {
                if (!emu.try_read_memory(env + i, data.data(), data.size()))
                {
                    return i;
                }

                if (data == empty)
                {
                    return i + data.size();
                }
            }

            return 0;
        }

        emulator_hook* install_env_hook(const std::shared_ptr<analysis_state>& state)
        {
            const auto process_params = get_process_params(state->win_emu_);

            auto install_env_access_hook = [state] {
                const auto env_ptr = get_environment_ptr(state->win_emu_);
                const auto env_size = get_environment_size(state->win_emu_.emu(), env_ptr);
                if (!env_size)
                {
                    state->env_data_hook_.remove();
                    return;
                }

                auto hook_handler = [state, env_ptr](cpu_interface&, const uint64_t address, const void*, const size_t size) {
                    const auto rip = state->win_emu_.emu().read_instruction_pointer();
                    const auto* mod = state->win_emu_.mod_manager.find_by_address(rip);
                    const auto is_main_access =
                        !mod || (mod == state->win_emu_.mod_manager.executable || state->modules_.contains(mod->name));

                    if (!is_main_access && !state->verbose_)
                    {
                        return;
                    }

                    if (state->concise_)
                    {
                        const auto count = ++state->env_module_cache_[mod ? mod->name : "<N/A>"];
                        if (count > 30 && count % 1000 != 0)
                        {
                            return;
                        }
                    }

                    state->context_.emit_observation<environment_access_event>([&](auto& event) {
                        event.main_access = is_main_access;
                        event.offset = address - env_ptr;
                        event.size = size;
                    });
                };

                state->env_data_hook_ = state->win_emu_.emu().hook_memory_read(env_ptr, env_size, std::move(hook_handler));
            };

            install_env_access_hook();

            auto& win_emu = state->win_emu_;
            return state->win_emu_.emu().hook_memory_write(
                process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment), 0x8,
                [&win_emu, install = std::move(install_env_access_hook)](cpu_interface&, const uint64_t address, const void*, size_t) {
                    const auto new_process_params = get_process_params(win_emu);

                    const auto target_address = new_process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment);

                    if (address == target_address)
                    {
                        install();
                    }
                });
        }

        void watch_system_objects(analysis_context& c, const std::set<std::string, std::less<>>& modules, const bool verbose,
                                  const bool concise)
        {
            auto& win_emu = *c.win_emu;
            win_emu.setup_process_if_necessary();

            auto emit_object_access = [&c](object_access_info info) {
                c.emit_observation<object_access_event>([&](auto& event) {
                    event.main_access = info.main_access;
                    event.type_name = std::move(info.type_name);
                    event.offset = info.offset;
                    event.size = info.size;
                    event.member_name = std::move(info.member_name);
                });
            };

            watch_object(win_emu, modules, *win_emu.current_thread().teb64, verbose, emit_object_access);
            watch_object(win_emu, modules, win_emu.process.peb64, verbose, emit_object_access);
            watch_object<KUSER_SHARED_DATA64>(win_emu, modules, kusd_mmio::address(), verbose, emit_object_access);

            auto state = std::make_shared<analysis_state>(c, modules, verbose, concise);

            state->params_hook_ =
                watch_object(win_emu, modules, win_emu.process.process_params64, verbose, emit_object_access, state->params_state_);
            state->ldr_hook_ = watch_object<PEB_LDR_DATA64>(win_emu, modules, win_emu.process.peb64.read().Ldr, verbose, emit_object_access,
                                                            state->ldr_state_);

            const auto update_env_hook = [state] {
                state->env_ptr_hook_ = install_env_hook(state); //
            };

            update_env_hook();

            win_emu.emu().hook_memory_write(
                win_emu.process.peb64.value() + offsetof(PEB64, ProcessParameters), 0x8,
                [state, emit_object_access, update_env = std::move(update_env_hook)](cpu_interface&, const uint64_t, const void*, size_t) {
                    const auto new_ptr = state->win_emu_.process.peb64.read().ProcessParameters;
                    state->params_hook_ = watch_object<RTL_USER_PROCESS_PARAMETERS64>(
                        state->win_emu_, state->modules_, new_ptr, state->verbose_, emit_object_access, state->params_state_);
                    update_env();
                });

            win_emu.emu().hook_memory_write(win_emu.process.peb64.value() + offsetof(PEB64, Ldr), 0x8,
                                            [state, emit_object_access](cpu_interface&, const uint64_t, const void*, size_t) {
                                                const auto new_ptr = state->win_emu_.process.peb64.read().Ldr;
                                                state->ldr_hook_ =
                                                    watch_object<PEB_LDR_DATA64>(state->win_emu_, state->modules_, new_ptr, state->verbose_,
                                                                                 emit_object_access, state->ldr_state_);
                                            });
        }

        bool read_yes_no_answer()
        {
            while (true)
            {
                const auto chr = static_cast<char>(getchar());
                if (chr == 'y')
                {
                    return true;
                }

                if (chr == 'n')
                {
                    return false;
                }
            }
        }

        std::vector<instruction_summary_entry> build_instruction_summary(const analysis_context& c)
        {
            std::map<uint64_t, std::vector<uint32_t>> instruction_counts{};

            for (const auto& [instruction, count] : c.instructions)
            {
                instruction_counts[count].push_back(instruction);
            }

            std::vector<instruction_summary_entry> entries{};
            for (const auto& [count, instructions] : instruction_counts)
            {
                for (const auto& instruction : instructions)
                {
                    const auto& e = c.win_emu;
                    auto& emu = e->emu();
                    const auto reg_cs = emu.reg<uint16_t>(x86_register::cs);
                    const auto handle = c.d.resolve_handle(emu, reg_cs);
                    const auto* mnemonic = cs_insn_name(handle, instruction);
                    entries.emplace_back(instruction_summary_entry{.mnemonic = mnemonic ? mnemonic : "<N/A>", .count = count});
                }
            }

            return entries;
        }

        void do_post_emulation_work(const analysis_context& c)
        {
            if (c.settings->instruction_summary)
            {
                c.emit_summary<instruction_summary_event>([&](auto& event) { event.entries = build_instruction_summary(c); });
            }

            if (c.settings->buffer_stdout)
            {
                c.emit_summary<buffered_stdout_event>([&](auto& event) { event.data = c.output; });
            }
        }

        void flush_reporters(const analysis_context& c)
        {
            for (auto* reporter : c.reporters)
            {
                reporter->flush();
            }
        }

        bool run_emulation(const analysis_context& c, const analysis_options& options)
        {
            auto& win_emu = *c.win_emu;

            std::atomic_uint32_t signals_received{0};
            utils::interupt_handler _{[&] {
                const auto value = signals_received++;
                if (value == 1)
                {
                    win_emu.log.log("Exit already requested. Press CTRL+C again to force kill!\n");
                }
                else if (value >= 2)
                {
                    _Exit(1);
                }

                win_emu.stop();
            }};

            std::optional<NTSTATUS> exit_status{};
#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
            const auto _1 = utils::finally([&] {
                debugger::handle_exit(win_emu, exit_status); //
            });
#endif

            auto emit_failure = [&](std::string message) {
                do_post_emulation_work(c);
                c.emit_summary<run_failed_event>([&](auto& event) {
                    event.rip = win_emu.emu().read_instruction_pointer();
                    event.message = std::move(message);
                });
                flush_reporters(c);
                return false;
            };

            try
            {
                if (options.use_gdb)
                {
                    const auto address = network::address{options.gdb_host, options.gdb_port};
                    win_emu.log.force_print(color::pink, "Waiting for GDB connection on %s...\n", address.to_string().c_str());

                    const auto should_stop = [&] { return signals_received > 0; };

                    win_x86_64_gdb_stub_handler handler{win_emu, should_stop};
                    gdb_stub::run_gdb_stub(address, handler);
                }
                else if (!options.minidump_path.empty())
                {
                    // For minidumps, don't start execution automatically; just report ready state
                    win_emu.log.print(color::green, "Minidump loaded successfully. Process state ready for analysis.\n");
                    c.emit_summary<run_finished_event>([&](auto& event) {
                        event.success = true;
                        event.exit_status = std::nullopt;
                    });
                    flush_reporters(c);
                    return true;
                }
                else
                {
#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
                    if (options.pause_before_start)
                    {
                        debugger::enter_breakpoint(win_emu, win_emu.mod_manager.executable->entry_point);
                    }
#endif
                    win_emu.start();
                }

                if (signals_received > 0)
                {
                    options.use_gdb = false;

                    win_emu.log.log("Do you want to create a snapshot? (y/n)\n");
                    const auto write_snapshot = read_yes_no_answer();

                    if (write_snapshot)
                    {
                        snapshot::write_emulator_snapshot(win_emu);
                    }
                }
            }
            catch (const gdb_stub::binding_error& e)
            {
                win_emu.log.error("Cannot bind to address %s\n", e.what());
                options.use_gdb = false;
                return emit_failure("Cannot bind to address "s + e.what());
            }
            catch (const std::exception& e)
            {
                return emit_failure(e.what());
            }
            catch (...)
            {
                return emit_failure("Unknown exception");
            }

            exit_status = win_emu.process.exit_status;
            if (!exit_status.has_value())
            {
                return emit_failure("Emulation terminated without status");
            }

            const auto success = *exit_status == STATUS_SUCCESS;
            do_post_emulation_work(c);
            win_emu.log.disable_output(false);
            c.emit_summary<run_finished_event>([&](auto& event) {
                event.success = success;
                event.exit_status = static_cast<uint32_t>(*exit_status);
            });
            flush_reporters(c);
            return success;
        }

        std::vector<std::u16string> parse_arguments(const std::span<const std::string_view> args)
        {
            std::vector<std::u16string> wide_args{};
            wide_args.reserve(args.size() - 1);

            for (size_t i = 1; i < args.size(); ++i)
            {
                const auto& arg = args[i];
                wide_args.emplace_back(arg.begin(), arg.end());
            }

            return wide_args;
        }

        emulator_settings create_emulator_settings(const analysis_options& options)
        {
            return {
                .use_relative_time = options.reproducible,
                .use_instruction_precision = !options.disable_instruction_precision,
                .vcpu_count = options.vcpu_count,
                .emulation_root = options.emulation_root,
                .registry_directory = options.registry_path,
                .path_mappings = options.path_mappings,
            };
        }

        hook_interface::memory_execution_hook_mode parse_memory_execution_hook_mode(const std::string_view mode)
        {
            if (mode == "auto")
            {
                return hook_interface::memory_execution_hook_mode::automatic;
            }

            if (mode == "int3")
            {
                return hook_interface::memory_execution_hook_mode::int3;
            }

            throw std::runtime_error("WHP memory execution hook mode must be auto or int3");
        }

        std::unique_ptr<x86_64_emulator> create_configured_backend(const analysis_options& options)
        {
            auto emu = options.backend ? create_x86_64_emulator(*options.backend, options.vcpu_count)
                                       : create_x86_64_emulator_from_environment(options.vcpu_count);
            emu->set_memory_execution_hook_mode(parse_memory_execution_hook_mode(options.whp_execution_hook_mode));
            return emu;
        }

        std::unique_ptr<windows_emulator> create_empty_emulator(const analysis_options& options)
        {
            const auto settings = create_emulator_settings(options);
            return std::make_unique<windows_emulator>(create_configured_backend(options), settings);
        }

        std::unique_ptr<windows_emulator> create_application_emulator(const analysis_options& options,
                                                                      const std::span<const std::string_view> args)
        {
            if (args.empty())
            {
                throw std::runtime_error("No args provided");
            }

            application_settings app_settings{
                .application = std::u8string(args[0].begin(), args[0].end()),
                .arguments = parse_arguments(args),
                .environment = options.environment,
            };

            const auto settings = create_emulator_settings(options);
            return std::make_unique<windows_emulator>(create_configured_backend(options), std::move(app_settings), settings);
        }

        std::unique_ptr<windows_emulator> setup_emulator(const analysis_options& options, const std::span<const std::string_view> args)
        {
            if (!options.dump.empty())
            {
                // load snapshot
                auto win_emu = create_empty_emulator(options);
                snapshot::load_emulator_snapshot(*win_emu, options.dump);
                return win_emu;
            }
            if (!options.minidump_path.empty())
            {
                // load minidump
                auto win_emu = create_empty_emulator(options);
                minidump_loader::load_minidump_into_emulator(*win_emu, options.minidump_path);
                return win_emu;
            }

            // default: load application
            return create_application_emulator(options, args);
        }

        const char* get_module_memory_region_name(const mapped_module& mod, const uint64_t address)
        {
            if (!mod.contains(address))
            {
                return "outside???";
            }

            uint64_t first_section = mod.image_base + mod.size_of_image;

            for (const auto& section : mod.sections)
            {
                first_section = std::min(first_section, section.region.start);

                if (is_within_start_and_length(address, section.region.start, section.region.length))
                {
                    return section.name.c_str();
                }
            }

            if (address < first_section)
            {
                return "header";
            }

            return "?";
        }

        bool run(const analysis_options& options, const std::span<const std::string_view> args)
        {
            analysis_context context{
                .settings = &options,
                .auto_break_before_call = options.break_call,
            };

            const auto concise_logging = options.concise_logging;
            const auto win_emu = setup_emulator(options, args);
            context.win_emu = win_emu.get();

            std::vector<std::unique_ptr<analysis_reporter>> reporters{};
            reporters.emplace_back(create_console_reporter(win_emu->log, console_reporter_settings{
                                                                             .silent = options.silent,
                                                                             .buffer_stdout = options.buffer_stdout,
                                                                             .prepend_call_count = options.prepend_call_count,
                                                                         }));

            if (!options.report_path.empty())
            {
                if (options.report_format != "jsonl")
                {
                    throw std::runtime_error("Unsupported report format: " + options.report_format);
                }

                reporters.emplace_back(create_jsonl_reporter(options.report_path));
            }

            context.reporters.reserve(reporters.size());
            for (const auto& reporter : reporters)
            {
                context.reporters.push_back(reporter.get());
            }

            win_emu->log.disable_output(concise_logging);

            std::vector<std::string> application_args{};
            if (!args.empty())
            {
                application_args.reserve(args.size() - 1);
                for (size_t i = 1; i < args.size(); ++i)
                {
                    application_args.emplace_back(args[i]);
                }
            }

            const auto* run_mode = "application";
            if (!options.minidump_path.empty())
            {
                run_mode = "minidump";
            }
            else if (!options.dump.empty())
            {
                run_mode = "snapshot";
            }

            context.emit_summary<run_started_event>([&](auto& event) {
                event.backend_name = win_emu->emu().get_name();
                event.mode = run_mode;
                event.application = args.empty() ? std::string{} : std::string(args[0]);
                event.arguments = std::move(application_args);
            });

            std::optional<tenet_tracer> tenet_tracer{};
            if (options.tenet_trace)
            {
                win_emu->log.log("Tenet Tracer enabled. Output: tenet_trace.log\n");
                tenet_tracer.emplace(*win_emu, "tenet_trace.log");
            }

            register_analysis_callbacks(context);
            watch_system_objects(context, options.modules, options.verbose_logging, options.concise_logging);

            const auto& exe = *win_emu->mod_manager.executable;
            const auto is_whp = win_emu->emu().get_name() == "Windows Hypervisor Platform";

            win_emu->emu().hook_instruction(x86_hookable_instructions::cpuid, [&] {
                auto& emu = win_emu->emu();

                const auto rip = emu.read_instruction_pointer();
                const auto leaf = emu.reg<uint32_t>(x86_register::eax);
                const auto mod = get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

                if (mod.has_value() && (!concise_logging || context.cpuid_cache.insert({rip, leaf}).second))
                {
                    context.emit_observation<cpuid_event>([&](auto& event) { event.leaf = leaf; });
                }

                if (leaf == 1 && !is_whp)
                {
                    // NOTE: We hard-code these values to disable SSE4.x and AVX
                    //       See: https://github.com/momo5502/sogen/issues/560
                    emu.reg<uint32_t>(x86_register::eax, 0x000906EA);
                    emu.reg<uint32_t>(x86_register::ebx, 0x00100800);
                    emu.reg<uint32_t>(x86_register::ecx, 0xEFE2F38F);
                    emu.reg<uint32_t>(x86_register::edx, 0xBFEBFBFF);

                    return instruction_hook_continuation::skip_instruction;
                }

                if (leaf == 0x40000000 && !is_whp)
                {
                    // Microsoft Hv vendor string
                    emu.reg<uint32_t>(x86_register::eax, 0x40000003);
                    emu.reg<uint32_t>(x86_register::ebx, 0x7263694d);
                    emu.reg<uint32_t>(x86_register::ecx, 0x666f736f);
                    emu.reg<uint32_t>(x86_register::edx, 0x76482074);

                    return instruction_hook_continuation::skip_instruction;
                }

                if (leaf == 0x40000003 && !is_whp)
                {
                    emu.reg<uint32_t>(x86_register::eax, 0x00000000);
                    emu.reg<uint32_t>(x86_register::ebx, 0x00000001);
                    emu.reg<uint32_t>(x86_register::ecx, 0x00000000);
                    emu.reg<uint32_t>(x86_register::edx, 0x00000000);

                    return instruction_hook_continuation::skip_instruction;
                }

                return instruction_hook_continuation::run_instruction;
            });

            if (options.log_foreign_module_access)
            {
                auto module_cache = std::make_shared<std::map<std::string, uint64_t>>();
                win_emu->emu().hook_memory_read(0, std::numeric_limits<uint64_t>::max(),
                                                [&, module_cache](cpu_interface&, const uint64_t address, const void*, size_t size) {
                                                    const auto rip = win_emu->emu().read_instruction_pointer();
                                                    const auto accessor =
                                                        get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

                                                    if (!accessor.has_value())
                                                    {
                                                        return;
                                                    }

                                                    const auto* mod = win_emu->mod_manager.find_by_address(address);
                                                    if (!mod || mod == *accessor)
                                                    {
                                                        return;
                                                    }

                                                    if (concise_logging)
                                                    {
                                                        const auto count = ++(*module_cache)[mod->name];
                                                        if (count > 30 && count % 100000 != 0)
                                                        {
                                                            return;
                                                        }
                                                    }

                                                    const auto* region_name = get_module_memory_region_name(*mod, address);
                                                    context.emit_observation<foreign_module_read_event>([&](auto& event) {
                                                        event.address = address;
                                                        event.size = size;
                                                        event.module_name = mod->name;
                                                        event.region_name = region_name;
                                                    });
                                                });
            }

            if (options.log_executable_access)
            {
                for (const auto& section : exe.sections)
                {
                    if ((section.region.permissions & memory_permission::exec) != memory_permission::exec)
                    {
                        continue;
                    }

                    const auto read_count = std::make_shared<uint64_t>(0);
                    const auto write_count = std::make_shared<uint64_t>(0);

                    auto read_handler = [&, section, concise_logging, read_count](cpu_interface&, const uint64_t address, const void*,
                                                                                  size_t size) {
                        const auto rip = win_emu->emu().read_instruction_pointer();
                        const auto accessor = get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

                        if (!accessor.has_value())
                        {
                            return;
                        }

                        if (concise_logging)
                        {
                            const auto count = ++*read_count;
                            if (count > 20 && count % 100000 != 0)
                            {
                                return;
                            }
                        }

                        context.emit_observation<executable_read_event>([&](auto& event) {
                            event.address = address;
                            event.size = size;
                            event.section_name = section.name;
                        });
                    };

                    auto write_handler = [&, section, concise_logging, write_count](cpu_interface&, const uint64_t address,
                                                                                    const void* value, size_t size) {
                        if (concise_logging)
                        {
                            const auto count = ++*write_count;
                            if (count > 30 && count % 100000 != 0)
                            {
                                return;
                            }
                        }

                        uint64_t int_value{};
                        memcpy(&int_value, value, std::min(size, sizeof(int_value)));
                        context.emit_observation<executable_write_event>([&](auto& event) {
                            event.address = address;
                            event.size = size;
                            event.value = int_value;
                            event.section_name = section.name;
                        });
                    };

                    win_emu->emu().hook_memory_read(section.region.start, section.region.length, std::move(read_handler));
                    win_emu->emu().hook_memory_write(section.region.start, section.region.length, std::move(write_handler));
                }
            }

            return run_emulation(context, options);
        }

        int run_main(int argc, char** argv)
        {
#ifndef _WIN32
            signal(SIGPIPE, SIG_IGN);
#endif

            CLI::App app{"Sogen Windows Analyzer"};

            // On Windows this resolves the UTF-8 arguments from the wide command line; elsewhere it is a no-op.
            argv = app.ensure_utf8(argv);

            // Stop parsing at the first positional (the application to analyze) and forward everything after it
            // verbatim to the emulated program.
            app.prefix_command();
            app.footer("Examples:\n"
                       "  analyzer -v -e path/to/root myapp.exe\n"
                       "  analyzer --report run.jsonl test-sample.exe\n"
                       "  analyzer -e path/to/root -p c:/analysis-sample.exe /path/to/sample.exe c:/analysis-sample.exe");

            analysis_options options{};

            auto* const debug_option = app.add_flag("-d,--debug", options.use_gdb, "Enable GDB debugging mode");
            app.add_option("--bind", options.gdb_host, "IP or hostname to bind to in GDB mode")->capture_default_str()->needs(debug_option);
            app.add_option("--port", options.gdb_port, "Port to listen to in GDB mode")->capture_default_str()->needs(debug_option);
            app.add_option("--break-call", options.break_call, "In GDB mode, stop before the specified traced function/syscall call")
                ->needs(debug_option);

            app.add_flag("-s,--silent", options.silent, "Silent mode");
            app.add_flag("-v,--verbose", options.verbose_logging, "Verbose logging");
            app.add_flag("-b,--buffer", options.buffer_stdout, "Buffer stdout");
            app.add_flag("-f,--foreign", options.log_foreign_module_access, "Log read access to foreign modules");
            app.add_flag("-c,--concise", options.concise_logging, "Concise logging");
            app.add_flag_callback(
                "--very-concise",
                [&] {
                    options.concise_logging = true;
                    options.skip_syscalls = true;
                    options.skip_generic_activity = true;
                },
                "Very concise logging");
            app.add_flag("-x,--exec", options.log_executable_access, "Log r/w access to executable memory");
            app.add_flag("-t,--tenet-trace", options.tenet_trace, "Enable Tenet tracer");
            app.add_flag("--first-exec", options.log_first_section_execution, "Print first executions of sections");
            app.add_flag("--inst-summary", options.instruction_summary, "Print a summary of executed instructions of the analyzed modules");
            app.add_flag("--skip-syscalls", options.skip_syscalls, "Skip the logging of regular syscalls");
            app.add_flag("--reproducible", options.reproducible, "Stub clocks and other mechanisms to make executions reproducible");
            app.add_flag("--no-inst-precision", options.disable_instruction_precision,
                         "Disable per-instruction precision (faster, less precise)");
            app.add_flag("--call-count", options.prepend_call_count, "Prefix function and syscall lines with a traced-call count");
#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
            app.add_flag("--break-start", options.pause_before_start, "Pause before executing the first instruction");
#endif

            app.add_option("-e,--emulation", options.emulation_root, "Set emulation root path");
            app.add_option("-a,--snapshot", options.dump, "Load snapshot dump from path");
            app.add_option("--minidump", options.minidump_path, "Load minidump from path");
            app.add_option("--report", options.report_path, "Write machine-readable analysis events to a file");
            app.add_option("--report-format", options.report_format, "Report format (supported: jsonl)")->capture_default_str();
            app.add_option("--whp-exec-hook", options.whp_execution_hook_mode, "WHP memory execution hook mode")
                ->capture_default_str()
                ->check(CLI::IsMember({"auto", "int3"}));
            app.add_option("-r,--registry", options.registry_path, "Set registry path");

            app.add_option("--vcpus", options.vcpu_count, "Number of virtual CPUs (requires a backend with multi-vCPU support)")
                ->capture_default_str();

            std::string backend_name{};
            app.add_option("--backend", backend_name, "Select CPU backend: unicorn, icicle, whp or kvm (overrides env)")
                ->check(CLI::IsMember({"unicorn", "icicle", "whp", "kvm"}));

            std::vector<std::string> tracked_modules{};
            app.add_option("-m,--module", tracked_modules, "Specify module(s) to track")->allow_extra_args(false);

            std::vector<std::string> ignored_functions{};
            app.add_option("-i,--ignore", ignored_functions, "Comma-separated list of functions to ignore")->allow_extra_args(false);

            std::vector<std::pair<std::string, std::string>> path_mappings{};
            app.add_option("-p,--path", path_mappings, "Map a Windows path to a host path")->type_name("SRC DST")->allow_extra_args(false);

            std::vector<std::pair<std::string, std::string>> environment{};
            app.add_option("--env", environment, "Set an environment variable")->type_name("NAME VALUE")->allow_extra_args(false);

            CLI11_PARSE(app, argc, argv);

            if (argc <= 1)
            {
                puts(app.help().c_str());
                return 1;
            }

            try
            {
                if (options.use_gdb && options.vcpu_count > 1)
                {
                    throw std::runtime_error("GDB debugging requires --vcpus 1");
                }

                if (!backend_name.empty())
                {
                    static const std::map<std::string, backend_type> backends{
                        {"unicorn", backend_type::unicorn},
                        {"icicle", backend_type::icicle},
                        {"whp", backend_type::whp},
                        {"kvm", backend_type::kvm},
                    };
                    options.backend = backends.at(backend_name);
                }

                for (auto& module_name : tracked_modules)
                {
                    options.modules.insert(std::move(module_name));
                }

                for (const auto& functions : ignored_functions)
                {
                    split_and_insert(options.ignored_functions, functions);
                }

                for (const auto& [source, target] : path_mappings)
                {
                    options.path_mappings[windows_path(source)] = std::filesystem::absolute(target);
                }

                for (const auto& [name, value] : environment)
                {
                    options.environment[std::u16string(name.begin(), name.end())] = std::u16string(value.begin(), value.end());
                }

                const auto application = app.remaining();
                const std::vector<std::string_view> args{application.begin(), application.end()};

                bool result{};

                do
                {
                    result = run(options, args);
                } while (options.use_gdb);

                return result ? 0 : 1;
            }
            catch (std::exception& e)
            {
                puts(e.what());
            }
            catch (...)
            {
                puts("An unknown exception occured");
            }

            return 1;
        }
    }

    int windows_main(const int argc, char** argv)
    {
        return run_main(argc, argv);
    }
}
