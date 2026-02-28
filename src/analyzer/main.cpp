#include "std_include.hpp"

#include <windows_emulator.hpp>
#include <backend_selection.hpp>
#include <win_x64_gdb_stub_handler.hpp>
#include <minidump_loader.hpp>
#include <scoped_hook.hpp>

#include "object_watching.hpp"
#include "snapshot.hpp"
#include "analysis.hpp"
#include "tenet_tracer.hpp"

#include <utils/finally.hpp>
#include <utils/interupt_handler.hpp>

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
#include <event_handler.hpp>
#endif

namespace
{
    struct analysis_options : analysis_settings
    {
        mutable bool use_gdb{false};
        std::string gdb_host{"127.0.0.1"};
        uint16_t gdb_port{28960};
        bool log_executable_access{false};
        bool log_foreign_module_access{false};
        bool tenet_trace{false};
        std::filesystem::path dump{};
        std::filesystem::path minidump_path{};
        std::string registry_path{"./registry"};
        std::string emulation_root{};
        std::unordered_map<windows_path, std::filesystem::path> path_mappings{};
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

#if !defined(__GNUC__) || defined(__clang__)
    struct analysis_state
    {
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

        analysis_state(windows_emulator& win_emu, std::set<std::string, std::less<>> modules, const bool verbose, const bool concise)
            : win_emu_(win_emu),
              env_data_hook_(win_emu.emu()),
              env_ptr_hook_(win_emu.emu()),
              params_hook_(win_emu.emu()),
              ldr_hook_(win_emu.emu()),
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

            auto hook_handler = [state, env_ptr](const uint64_t address, const void*, const size_t size) {
                const auto rip = state->win_emu_.emu().read_instruction_pointer();
                const auto* mod = state->win_emu_.mod_manager.find_by_address(rip);
                const auto is_main_access = !mod || (mod == state->win_emu_.mod_manager.executable || state->modules_.contains(mod->name));

                if (!is_main_access && !state->verbose_)
                {
                    return;
                }

                if (state->concise_)
                {
                    const auto count = ++state->env_module_cache_[mod->name];
                    if (count > 100 && count % 1000 != 0)
                    {
                        return;
                    }
                }

                const auto offset = address - env_ptr;
                const auto* mod_name = mod ? mod->name.c_str() : "<N/A>";
                state->win_emu_.log.print(is_main_access ? color::green : color::dark_gray,
                                          "Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)\n", offset, size, rip, mod_name);
            };

            state->env_data_hook_ = state->win_emu_.emu().hook_memory_read(env_ptr, env_size, std::move(hook_handler));
        };

        install_env_access_hook();

        auto& win_emu = state->win_emu_;
        return state->win_emu_.emu().hook_memory_write(
            process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment), 0x8,
            [&win_emu, install = std::move(install_env_access_hook)](const uint64_t address, const void*, size_t) {
                const auto new_process_params = get_process_params(win_emu);

                const auto target_address = new_process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment);

                if (address == target_address)
                {
                    install();
                }
            });
    }
#endif

    void watch_system_objects(windows_emulator& win_emu, const std::set<std::string, std::less<>>& modules, const bool verbose,
                              const bool concise)
    {
        win_emu.setup_process_if_necessary();

        (void)win_emu;
        (void)modules;
        (void)verbose;
        (void)concise;

#if !defined(__GNUC__) || defined(__clang__)
        watch_object(win_emu, modules, *win_emu.current_thread().teb64, verbose);
        watch_object(win_emu, modules, win_emu.process.peb64, verbose);
        watch_object<KUSER_SHARED_DATA64>(win_emu, modules, kusd_mmio::address(), verbose);

        auto state = std::make_shared<analysis_state>(win_emu, modules, verbose, concise);

        state->params_hook_ = watch_object(win_emu, modules, win_emu.process.process_params64, verbose, state->params_state_);
        state->ldr_hook_ = watch_object<PEB_LDR_DATA64>(win_emu, modules, win_emu.process.peb64.read().Ldr, verbose, state->ldr_state_);

        const auto update_env_hook = [state] {
            state->env_ptr_hook_ = install_env_hook(state); //
        };

        update_env_hook();

        win_emu.emu().hook_memory_write(win_emu.process.peb64.value() + offsetof(PEB64, ProcessParameters), 0x8,
                                        [state, update_env = std::move(update_env_hook)](const uint64_t, const void*, size_t) {
                                            const auto new_ptr = state->win_emu_.process.peb64.read().ProcessParameters;
                                            state->params_hook_ = watch_object<RTL_USER_PROCESS_PARAMETERS64>(
                                                state->win_emu_, state->modules_, new_ptr, state->verbose_, state->params_state_);
                                            update_env();
                                        });

        win_emu.emu().hook_memory_write(
            win_emu.process.peb64.value() + offsetof(PEB64, Ldr), 0x8, [state](const uint64_t, const void*, size_t) {
                const auto new_ptr = state->win_emu_.process.peb64.read().Ldr;
                state->ldr_hook_ =
                    watch_object<PEB_LDR_DATA64>(state->win_emu_, state->modules_, new_ptr, state->verbose_, state->ldr_state_);
            });
#endif
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

    void print_instruction_summary(const analysis_context& c)
    {
        std::map<uint64_t, std::vector<uint32_t>> instruction_counts{};

        for (const auto& [instruction, count] : c.instructions)
        {
            instruction_counts[count].push_back(instruction);
        }

        c.win_emu->log.print(color::white, "Instruction summary:\n");

        for (const auto& [count, instructions] : instruction_counts)
        {
            for (const auto& instruction : instructions)
            {
                const auto& e = c.win_emu;
                auto& emu = e->emu();
                const auto reg_cs = emu.reg<uint16_t>(x86_register::cs);
                const auto handle = c.d.resolve_handle(emu, reg_cs);
                const auto* mnemonic = cs_insn_name(handle, instruction);
                c.win_emu->log.print(color::white, "%s: %" PRIu64 "\n", mnemonic, count);
            }
        }
    }

    void do_post_emulation_work(const analysis_context& c)
    {
        if (c.settings->instruction_summary)
        {
            print_instruction_summary(c);
        }

        if (c.settings->buffer_stdout)
        {
            c.win_emu->log.info("%.*s%s", static_cast<int>(c.output.size()), c.output.data(), c.output.ends_with("\n") ? "" : "\n");
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
#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
        const auto _1 = utils::finally([&] {
            debugger::handle_exit(win_emu, exit_status); //
        });
#endif

        try
        {
            if (options.use_gdb)
            {
                const auto address = network::address{options.gdb_host, options.gdb_port};
                win_emu.log.force_print(color::pink, "Waiting for GDB connection on %s...\n", address.to_string().c_str());

                const auto should_stop = [&] { return signals_received > 0; };

                win_x64_gdb_stub_handler handler{win_emu, should_stop};
                gdb_stub::run_gdb_stub(address, handler);
            }
            else if (!options.minidump_path.empty())
            {
                // For minidumps, don't start execution automatically; just report ready state
                win_emu.log.print(color::green, "Minidump loaded successfully. Process state ready for analysis.\n");
                return true; // Return success without starting emulation
            }
            else
            {
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
        catch (const std::exception& e)
        {
            do_post_emulation_work(c);
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 " - %s\n", win_emu.emu().read_instruction_pointer(), e.what());
            throw;
        }
        catch (...)
        {
            do_post_emulation_work(c);
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 "\n", win_emu.emu().read_instruction_pointer());
            throw;
        }

        exit_status = win_emu.process.exit_status;
        if (!exit_status.has_value())
        {
            do_post_emulation_work(c);
            win_emu.log.error("Emulation terminated without status!\n");
            return false;
        }

        const auto success = *exit_status == STATUS_SUCCESS;

        if (!options.silent)
        {
            do_post_emulation_work(c);
            win_emu.log.disable_output(false);
            win_emu.log.print(success ? color::green : color::red, "Emulation terminated with status: %X\n", *exit_status);
        }

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
            .emulation_root = options.emulation_root,
            .registry_directory = options.registry_path,
            .path_mappings = options.path_mappings,
        };
    }

    std::unique_ptr<windows_emulator> create_empty_emulator(const analysis_options& options)
    {
        const auto settings = create_emulator_settings(options);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), settings);
    }

    std::unique_ptr<windows_emulator> create_application_emulator(const analysis_options& options,
                                                                  const std::span<const std::string_view> args)
    {
        if (args.empty())
        {
            throw std::runtime_error("No args provided");
        }

        application_settings app_settings{
            .application = args[0],
            .arguments = parse_arguments(args),
        };

        const auto settings = create_emulator_settings(options);
        return std::make_unique<windows_emulator>(create_x86_64_emulator(), std::move(app_settings), settings);
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
        };

        const auto concise_logging = options.concise_logging;

        const auto win_emu = setup_emulator(options, args);
        context.win_emu = win_emu.get();

        win_emu->log.disable_output(concise_logging || options.silent);

        if (!options.silent)
        {
            win_emu->log.force_print(color::gray, "Using emulator backend: %s\n", win_emu->emu().get_name().c_str());
        }

        std::optional<tenet_tracer> tenet_tracer{};
        if (options.tenet_trace)
        {
            win_emu->log.log("Tenet Tracer enabled. Output: tenet_trace.log\n");
            tenet_tracer.emplace(*win_emu, "tenet_trace.log");
        }

        register_analysis_callbacks(context);
        watch_system_objects(*win_emu, options.modules, options.verbose_logging, options.concise_logging);

        const auto& exe = *win_emu->mod_manager.executable;

        win_emu->emu().hook_instruction(x86_hookable_instructions::cpuid, [&] {
            auto& emu = win_emu->emu();

            const auto rip = emu.read_instruction_pointer();
            const auto leaf = emu.reg<uint32_t>(x86_register::eax);
            const auto mod = get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

            if (mod.has_value() && (!concise_logging || context.cpuid_cache.insert({rip, leaf}).second))
            {
                win_emu->log.print(color::blue, "Executing CPUID instruction with leaf 0x%X at 0x%" PRIx64 " (%s)\n", leaf, rip,
                                   (*mod) ? (*mod)->name.c_str() : "<N/A>");
            }

            if (leaf == 1)
            {
                // NOTE: We hard-code these values to disable SSE4.x and AVX
                //       See: https://github.com/momo5502/sogen/issues/560
                emu.reg<uint32_t>(x86_register::eax, 0x000906EA);
                emu.reg<uint32_t>(x86_register::ebx, 0x00100800);
                emu.reg<uint32_t>(x86_register::ecx, 0xEFE2F38F);
                emu.reg<uint32_t>(x86_register::edx, 0xBFEBFBFF);

                return instruction_hook_continuation::skip_instruction;
            }

            return instruction_hook_continuation::run_instruction;
        });

        if (options.log_foreign_module_access)
        {
            auto module_cache = std::make_shared<std::map<std::string, uint64_t>>();
            win_emu->emu().hook_memory_read(
                0, std::numeric_limits<uint64_t>::max(), [&, module_cache](const uint64_t address, const void*, size_t size) {
                    const auto rip = win_emu->emu().read_instruction_pointer();
                    const auto accessor = get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

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
                        if (count > 100 && count % 100000 != 0)
                        {
                            return;
                        }
                    }

                    const auto* region_name = get_module_memory_region_name(*mod, address);

                    win_emu->log.print(color::pink, "Reading %zd bytes from module %s at 0x%" PRIx64 " (%s) via 0x%" PRIx64 " (%s)\n", size,
                                       mod->name.c_str(), address, region_name, rip, (*accessor) ? (*accessor)->name.c_str() : "<N/A>");
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

                auto read_handler = [&, section, concise_logging, read_count](const uint64_t address, const void*, size_t size) {
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

                    win_emu->log.print(color::green,
                                       "Reading %zd bytes from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 " (%s)\n", size,
                                       section.name.c_str(), address, rip, (*accessor) ? (*accessor)->name.c_str() : "<N/A>");
                };

                const auto write_handler = [&, section, concise_logging, write_count](const uint64_t address, const void* value,
                                                                                      size_t size) {
                    const auto rip = win_emu->emu().read_instruction_pointer();

                    if (concise_logging)
                    {
                        const auto count = ++*write_count;
                        if (count > 100 && count % 100000 != 0)
                        {
                            return;
                        }
                    }

                    uint64_t int_value{};
                    memcpy(&int_value, value, std::min(size, sizeof(int_value)));

                    win_emu->log.print(color::blue,
                                       "Writing %zd bytes with value 0x%" PRIX64 " to executable section %s at 0x%" PRIx64 " via 0x%" PRIx64
                                       " (%s)\n",
                                       size, int_value, section.name.c_str(), address, rip, win_emu->mod_manager.find_name(rip));
                };

                win_emu->emu().hook_memory_read(section.region.start, section.region.length, std::move(read_handler));
                win_emu->emu().hook_memory_write(section.region.start, section.region.length, std::move(write_handler));
            }
        }

        return run_emulation(context, options);
    }

    std::vector<std::string_view> bundle_arguments(const int argc, char** argv)
    {
        std::vector<std::string_view> args{};

        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }

        return args;
    }

    void print_help()
    {
        printf("Usage: analyzer [options] [application] [args...]\n\n");
        printf("Options:\n");
        printf("  -h, --help                Show this help message\n");
        printf("  -d, --debug               Enable GDB debugging mode\n");
        printf("  --bind <ip>               IP or hostname to bind to in GDB mode (default: 127.0.0.1)\n");
        printf("  --port <port>             Port to listen to in GDB mode (default: 28960)\n");
        printf("  -s, --silent              Silent mode\n");
        printf("  -v, --verbose             Verbose logging\n");
        printf("  -b, --buffer              Buffer stdout\n");
        printf("  -f, --foreign             Log read access to foreign modules\n");
        printf("  -c, --concise             Concise logging\n");
        printf("  -x, --exec                Log r/w access to executable memory\n");
        printf("  -m, --module <module>     Specify module to track\n");
        printf("  -e, --emulation <path>    Set emulation root path\n");
        printf("  -a, --snapshot <path>     Load snapshot dump from path\n");
        printf("  --minidump <path>         Load minidump from path\n");
        printf("  -t, --tenet-trace         Enable Tenet tracer\n");
        printf("  -i, --ignore <funcs>      Comma-separated list of functions to ignore\n");
        printf("  -p, --path <src> <dst>    Map Windows path to host path\n");
        printf("  -r, --registry <path>     Set registry path (default: ./registry)\n\n");
        printf("  -is, --inst-summary       Print a summary of executed instructions of the analyzed modules\n");
        printf("  -ss, --skip-syscalls      Skip the logging of regular syscalls\n");
        printf("Examples:\n");
        printf("  analyzer -v -e path/to/root myapp.exe\n");
        printf("  analyzer -e path/to/root -p c:/analysis-sample.exe /path/to/sample.exe c:/analysis-sample.exe\n");
    }

    analysis_options parse_options(std::vector<std::string_view>& args)
    {
        analysis_options options{};
        bool require_debug_option = false;

        while (!args.empty())
        {
            auto arg_it = args.begin();
            const auto& arg = *arg_it;

            if (arg == "-h" || arg == "--help")
            {
                print_help();
                std::exit(0);
            }

            if (arg == "-d" || arg == "--debug")
            {
                options.use_gdb = true;
            }
            else if (arg == "--bind")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No IP provided after --bind");
                }

                arg_it = args.erase(arg_it);
                options.gdb_host = args[0];
                require_debug_option = true;
            }
            else if (arg == "--port")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No port number provided after --port");
                }

                arg_it = args.erase(arg_it);

                char* end_ptr = nullptr;
                const auto port = strtoull(std::string(args[0]).c_str(), &end_ptr, 10);

                if (*end_ptr)
                {
                    throw std::runtime_error("Bad port number");
                }

                if (port > 65535)
                {
                    throw std::runtime_error("Port number should be in range 0-65535");
                }

                options.gdb_port = static_cast<uint16_t>(port);
                require_debug_option = true;
            }
            else if (arg == "-s" || arg == "--silent")
            {
                options.silent = true;
            }
            else if (arg == "-v" || arg == "--verbose")
            {
                options.verbose_logging = true;
            }
            else if (arg == "-b" || arg == "--buffer")
            {
                options.buffer_stdout = true;
            }
            else if (arg == "-x" || arg == "--exec")
            {
                options.log_executable_access = true;
            }
            else if (arg == "-f" || arg == "--foreign")
            {
                options.log_foreign_module_access = true;
            }
            else if (arg == "-c" || arg == "--concise")
            {
                options.concise_logging = true;
            }
            else if (arg == "-t" || arg == "--tenet-trace")
            {
                options.tenet_trace = true;
            }
            else if (arg == "-is" || arg == "--inst-summary")
            {
                options.instruction_summary = true;
            }
            else if (arg == "-ss" || arg == "--skip-syscalls")
            {
                options.skip_syscalls = true;
            }
            else if (arg == "-m" || arg == "--module")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No module provided after -m/--module");
                }

                arg_it = args.erase(arg_it);
                options.modules.insert(std::string(args[0]));
            }
            else if (arg == "-e" || arg == "--emulation")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No emulation root path provided after -e/--emulation");
                }
                arg_it = args.erase(arg_it);
                options.emulation_root = args[0];
            }
            else if (arg == "-a" || arg == "--snapshot")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No dump path provided after -a/--snapshot");
                }
                arg_it = args.erase(arg_it);
                options.dump = args[0];
            }
            else if (arg == "--minidump")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No minidump path provided after --minidump");
                }
                arg_it = args.erase(arg_it);
                options.minidump_path = args[0];
            }
            else if (arg == "-i" || arg == "--ignore")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No ignored function(s) provided after -i/--ignore");
                }
                arg_it = args.erase(arg_it);
                split_and_insert(options.ignored_functions, args[0]);
            }
            else if (arg == "-p" || arg == "--path")
            {
                if (args.size() < 3)
                {
                    throw std::runtime_error("No path mapping provided after -p/--path");
                }
                arg_it = args.erase(arg_it);
                windows_path source = args[0];
                arg_it = args.erase(arg_it);
                std::filesystem::path target = std::filesystem::absolute(args[0]);

                options.path_mappings[std::move(source)] = std::move(target);
            }
            else if (arg == "-r" || arg == "--registry")
            {
                if (args.size() < 2)
                {
                    throw std::runtime_error("No registry path provided after -r/--registry");
                }
                arg_it = args.erase(arg_it);
                options.registry_path = args[0];
            }
            else
            {
                break;
            }

            args.erase(arg_it);
        }

        if (require_debug_option && !options.use_gdb)
        {
            throw std::runtime_error("--bind or --port options used without -d");
        }

        return options;
    }

    int run_main(const int argc, char** argv)
    {
        try
        {
            auto args = bundle_arguments(argc, argv);
            if (args.empty())
            {
                print_help();
                return 1;
            }

            const auto options = parse_options(args);

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

int main(const int argc, char** argv)
{
    return run_main(argc, argv);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return run_main(__argc, __argv);
}
#endif
