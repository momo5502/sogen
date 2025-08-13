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

#ifdef OS_EMSCRIPTEN
#include <event_handler.hpp>
#endif

namespace
{
    struct analysis_options : analysis_settings
    {
        mutable bool use_gdb{false};
        bool log_executable_access{false};
        bool tenet_trace{false};
        std::filesystem::path dump{};
        std::filesystem::path minidump_path{};
        std::string registry_path{"./registry"};
        std::string emulation_root{};
        std::unordered_map<windows_path, std::filesystem::path> path_mappings{};
    };

    void split_and_insert(std::set<std::string, std::less<>>& container, const std::string_view str,
                          const char splitter = ',')
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
        std::set<std::string, std::less<>> modules_;
        bool verbose_;

        analysis_state(windows_emulator& win_emu, std::set<std::string, std::less<>> modules, const bool verbose)
            : win_emu_(win_emu),
              env_data_hook_(win_emu.emu()),
              env_ptr_hook_(win_emu.emu()),
              params_hook_(win_emu.emu()),
              modules_(std::move(modules)),
              verbose_(verbose)
        {
        }
    };

    emulator_object<RTL_USER_PROCESS_PARAMETERS64> get_process_params(windows_emulator& win_emu)
    {
        const auto peb = win_emu.process.peb.read();
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
                const auto is_main_access =
                    !mod || (mod == state->win_emu_.mod_manager.executable || state->modules_.contains(mod->name));

                if (!is_main_access && !state->verbose_)
                {
                    return;
                }

                const auto offset = address - env_ptr;
                const auto* mod_name = mod ? mod->name.c_str() : "<N/A>";
                state->win_emu_.log.print(is_main_access ? color::green : color::dark_gray,
                                          "Environment access: 0x%" PRIx64 " (0x%zX) at 0x%" PRIx64 " (%s)\n", offset,
                                          size, rip, mod_name);
            };

            state->env_data_hook_ = state->win_emu_.emu().hook_memory_read(env_ptr, env_size, std::move(hook_handler));
        };

        install_env_access_hook();

        auto& win_emu = state->win_emu_;
        return state->win_emu_.emu().hook_memory_write(
            process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment), 0x8,
            [&win_emu, install = std::move(install_env_access_hook)](const uint64_t address, const void*, size_t) {
                const auto new_process_params = get_process_params(win_emu);

                const auto target_address =
                    new_process_params.value() + offsetof(RTL_USER_PROCESS_PARAMETERS64, Environment);

                if (address == target_address)
                {
                    install();
                }
            });
    }
#endif

    void watch_system_objects(windows_emulator& win_emu, const std::set<std::string, std::less<>>& modules,
                              const bool verbose)
    {
        win_emu.setup_process_if_necessary();

        (void)win_emu;
        (void)modules;
        (void)verbose;

#if !defined(__GNUC__) || defined(__clang__)
        watch_object(win_emu, modules, *win_emu.current_thread().teb, verbose);
        watch_object(win_emu, modules, win_emu.process.peb, verbose);
        watch_object(win_emu, modules, emulator_object<KUSER_SHARED_DATA64>{win_emu.emu(), kusd_mmio::address()},
                     verbose);

        auto state = std::make_shared<analysis_state>(win_emu, modules, verbose);

        state->params_hook_ = watch_object(win_emu, modules, win_emu.process.process_params, verbose);

        const auto update_env_hook = [state] {
            state->env_ptr_hook_ = install_env_hook(state); //
        };

        update_env_hook();

        win_emu.emu().hook_memory_write(
            win_emu.process.peb.value() + offsetof(PEB64, ProcessParameters), 0x8,
            [state, update_env = std::move(update_env_hook)](const uint64_t address, const void*, size_t) {
                const auto target_address = state->win_emu_.process.peb.value() + offsetof(PEB64, ProcessParameters);

                if (address != target_address)
                {
                    return;
                }

                const emulator_object<RTL_USER_PROCESS_PARAMETERS64> obj{
                    state->win_emu_.emu(),
                    state->win_emu_.emu().read_memory<uint64_t>(address),
                };

                state->params_hook_ = watch_object(state->win_emu_, state->modules_, obj, state->verbose_);
                update_env();
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

    void do_post_emulation_work(const analysis_context& c)
    {
        if (c.settings->buffer_stdout)
        {
            c.win_emu->log.info("%.*s%s", static_cast<int>(c.output.size()), c.output.data(),
                                c.output.ends_with("\n") ? "" : "\n");
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
                win_emu.log.log("Exit already requested. Press CTRL+C again to force kill!");
            }
            else if (value >= 2)
            {
                _Exit(1);
            }

            win_emu.stop();
        }};

        std::optional<NTSTATUS> exit_status{};
#ifdef OS_EMSCRIPTEN
        const auto _1 = utils::finally([&] {
            debugger::handle_exit(win_emu, exit_status); //
        });
#endif

        try
        {
            if (options.use_gdb)
            {
                const auto* address = "127.0.0.1:28960";
                win_emu.log.print(color::pink, "Waiting for GDB connection on %s...\n", address);

                const auto should_stop = [&] { return signals_received > 0; };

                win_x64_gdb_stub_handler handler{win_emu, should_stop};
                gdb_stub::run_gdb_stub(network::address{"0.0.0.0:28960", AF_INET}, handler);
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
            win_emu.log.error("Emulation failed at: 0x%" PRIx64 " - %s\n", win_emu.emu().read_instruction_pointer(),
                              e.what());
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
            win_emu.log.print(success ? color::green : color::red, "Emulation terminated with status: %X\n",
                              *exit_status);
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

    std::unique_ptr<windows_emulator> setup_emulator(const analysis_options& options,
                                                     const std::span<const std::string_view> args)
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

    bool run(const analysis_options& options, const std::span<const std::string_view> args)
    {
        analysis_context context{
            .settings = &options,
        };

        const auto win_emu = setup_emulator(options, args);
        win_emu->log.disable_output(options.concise_logging || options.silent);
        context.win_emu = win_emu.get();

        win_emu->log.log("Using emulator: %s\n", win_emu->emu().get_name().c_str());

        std::optional<tenet_tracer> tenet_tracer{};
        if (options.tenet_trace)
        {
            win_emu->log.log("Tenet Tracer enabled. Output: tenet_trace.log\n");
            tenet_tracer.emplace(*win_emu, "tenet_trace.log");
        }

        register_analysis_callbacks(context);
        watch_system_objects(*win_emu, options.modules, options.verbose_logging);

        const auto& exe = *win_emu->mod_manager.executable;

        const auto concise_logging = !options.verbose_logging;

        win_emu->emu().hook_instruction(x86_hookable_instructions::cpuid, [&] {
            const auto rip = win_emu->emu().read_instruction_pointer();
            auto* mod = get_module_if_interesting(win_emu->mod_manager, options.modules, rip);

            if (mod)
            {
                const auto leaf = win_emu->emu().reg<uint32_t>(x86_register::eax);
                win_emu->log.print(color::blue, "Executing CPUID instruction with leaf 0x%X at 0x%" PRIx64 " (%s)\n",
                                   leaf, rip, mod->name.c_str());
            }

            return instruction_hook_continuation::run_instruction;
        });

        if (options.log_executable_access)
        {
            for (const auto& section : exe.sections)
            {
                if ((section.region.permissions & memory_permission::exec) != memory_permission::exec)
                {
                    continue;
                }

                auto read_handler = [&, section, concise_logging](const uint64_t address, const void*, size_t) {
                    const auto rip = win_emu->emu().read_instruction_pointer();
                    if (!win_emu->mod_manager.executable->is_within(rip))
                    {
                        return;
                    }

                    if (concise_logging)
                    {
                        static uint64_t count{0};
                        ++count;
                        if (count > 100 && count % 100000 != 0)
                        {
                            return;
                        }
                    }

                    win_emu->log.print(color::green,
                                       "Reading from executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 "\n",
                                       section.name.c_str(), address, rip);
                };

                const auto write_handler = [&, section, concise_logging](const uint64_t address, const void*, size_t) {
                    const auto rip = win_emu->emu().read_instruction_pointer();
                    if (!win_emu->mod_manager.executable->is_within(rip))
                    {
                        return;
                    }

                    if (concise_logging)
                    {
                        static uint64_t count{0};
                        ++count;
                        if (count > 100 && count % 100000 != 0)
                        {
                            return;
                        }
                    }

                    win_emu->log.print(color::blue,
                                       "Writing to executable section %s at 0x%" PRIx64 " via 0x%" PRIx64 "\n",
                                       section.name.c_str(), address, rip);
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
        printf("  -s, --silent              Silent mode\n");
        printf("  -v, --verbose             Verbose logging\n");
        printf("  -b, --buffer              Buffer stdout\n");
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
        printf("Examples:\n");
        printf("  analyzer -v -e path/to/root myapp.exe\n");
        printf("  analyzer -e path/to/root -p c:/analysis-sample.exe /path/to/sample.exe c:/analysis-sample.exe\n");
    }

    analysis_options parse_options(std::vector<std::string_view>& args)
    {
        analysis_options options{};

        while (!args.empty())
        {
            auto arg_it = args.begin();
            const auto& arg = *arg_it;

            if (arg == "-h" || arg == "--help")
            {
                print_help();
                std::exit(0);
            }
            else if (arg == "-d" || arg == "--debug")
            {
                options.use_gdb = true;
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
            else if (arg == "-c" || arg == "--concise")
            {
                options.concise_logging = true;
            }
            else if (arg == "-t" || arg == "--tenet-trace")
            {
                options.tenet_trace = true;
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
