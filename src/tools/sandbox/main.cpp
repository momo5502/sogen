#include <windows_emulator.hpp>
#include <subprocess_process_manager.hpp>
#include <emulator_process_target.hpp>
#include <inherited_pipes.hpp>
#include <registry/registry_file.hpp>
#include <snapshot.hpp>
#include <utils/finally.hpp>
#include <utils/executable_path.hpp>
#ifdef _WIN32
#include <whp_x86_64_emulator.hpp>
#include <utils/win.hpp>
#elif defined(__ANDROID__)
#include <backend_selection.hpp>
#else
#include <kvm_x86_64_emulator.hpp>
#endif

#include <utils/interupt_handler.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sogen::sandbox
{
    namespace
    {
        std::filesystem::path get_current_binary_dir()
        {
            return utils::get_current_executable_path().parent_path();
        }

        std::vector<std::u16string> parse_arguments(const std::span<const std::string_view> args)
        {
            std::vector<std::u16string> wide_args{};
            wide_args.reserve(args.empty() ? 0 : args.size() - 1);

            for (size_t i = 1; i < args.size(); ++i)
            {
                const auto& arg = args[i];
                wide_args.emplace_back(arg.begin(), arg.end());
            }

            return wide_args;
        }

        // Without an emulation root the path is empty, which absolute() rejects on Windows.
        std::string describe_path(const std::filesystem::path& path)
        {
            return path.empty() ? std::string{} : std::filesystem::absolute(path).string();
        }

        std::string describe_configuration(const emulator_settings& settings, const std::vector<std::filesystem::path>& registry_files)
        {
            std::string configuration{};
            configuration += "emulation-root=" + describe_path(settings.emulation_root) + "\n";
            configuration += "root-mode=" + std::string{to_string(settings.root_mode)} + "\n";
            configuration += "registry=" + describe_path(settings.registry_directory) + "\n";
            configuration += "network=" + std::string{network::to_string(settings.network)} + "\n";
            configuration += std::string{"spoof-gpu-features="} + (settings.spoof_gpu_features ? "on" : "off") + "\n";

            for (const auto& mount : settings.mounts)
            {
                configuration += "mount=" + render_mount_spec(mount) + "\n";
            }

            for (const auto& file : registry_files)
            {
                configuration += "reg-file=" + describe_path(file) + "\n";
            }

            return configuration;
        }

        int run(const std::span<const std::string_view> args, const filesystem_mode root_mode, std::vector<mount_spec> mounts,
                const std::filesystem::path& emulation_root, const std::filesystem::path& registry_directory,
                const std::vector<std::filesystem::path>& registry_files,
                const std::vector<std::pair<std::string, std::string>>& environment, const network::network_mode network_mode,
                const bool spoof_gpu_features, const std::filesystem::path& snapshot_directory, const std::filesystem::path& resume_file,
                managed_process_connection* managed_connection)
        {
            application_settings app_settings{
                .application = args.empty() ? std::u8string{} : std::u8string(args[0].begin(), args[0].end()),
                .arguments = parse_arguments(args),
            };
            for (const auto& [name, value] : environment)
            {
                app_settings.environment.insert_or_assign(u8_to_u16(name), u8_to_u16(value));
            }
            if (managed_connection)
            {
                const auto& request = managed_connection->request();
                app_settings.argument0 = u8_to_u16(request.argument0);
                app_settings.command_line = u8_to_u16(request.command_line);
                app_settings.process_id = request.process_id;
                app_settings.thread_id = request.thread_id;
                if (!request.working_directory.empty())
                {
                    app_settings.working_directory = windows_path(u8_to_u16(request.working_directory));
                }
                for (const auto& [name, value] : request.environment)
                {
                    app_settings.environment.insert_or_assign(u8_to_u16(name), u8_to_u16(value));
                }
            }

#ifdef _WIN32
            // One vCPU per host core; WHP supports at most 64 per partition. EMULATOR_VCPU_COUNT overrides. Snapshots only
            // support a single vCPU so far.
            const auto resumes = !resume_file.empty();
            auto vcpu_count = resumes ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
            if (const char* env = std::getenv("EMULATOR_VCPU_COUNT"); env != nullptr && env[0] != '\0')
            {
                vcpu_count = std::clamp(static_cast<uint32_t>(std::strtoul(env, nullptr, 10)), 1u, 64u);
            }

            if (resumes && vcpu_count != 1)
            {
                throw std::runtime_error("Resuming supports only one vCPU");
            }
#endif

            emulator_settings settings{
                // The sandbox favours speed over exact timing: read the TSC and KUSER_SHARED_DATA natively instead of
                // trapping every access (the analyzer keeps interception on).
                .intercept_rdtsc = false,
                .intercept_kusd = false,
                .registry_directory = registry_directory.empty() ? get_current_binary_dir() / "registry" : registry_directory,
                .network = network_mode,
                .spoof_gpu_features = spoof_gpu_features,
            };

            if (!emulation_root.empty())
            {
                settings.emulation_root = emulation_root;
            }
            else if (const char* root = std::getenv("EMULATOR_ROOT"); root != nullptr && root[0] != '\0')
            {
                settings.emulation_root = root;
            }

            settings.root_mode = root_mode;
            settings.mounts = std::move(mounts);

            subprocess_process_manager process_manager{utils::get_current_executable_path(), [&](const auto port, const auto& token) {
                                                           std::vector<std::string> arguments{"--managed-process-port",
                                                                                              std::to_string(port),
                                                                                              "--managed-process-token",
                                                                                              token,
                                                                                              "--registry",
                                                                                              settings.registry_directory.string()};
                                                           arguments.emplace_back("--network");
                                                           arguments.emplace_back(network::to_string(network_mode));
                                                           if (spoof_gpu_features)
                                                           {
                                                               arguments.emplace_back("--spoof-gpu-features");
                                                           }
                                                           if (!settings.emulation_root.empty())
                                                           {
                                                               arguments.emplace_back("--emulation");
                                                               arguments.push_back(settings.emulation_root.string());
                                                           }
                                                           arguments.emplace_back("--root-mode");
                                                           arguments.emplace_back(to_string(settings.root_mode));
                                                           for (const auto& mount : settings.mounts)
                                                           {
                                                               arguments.emplace_back("--mount");
                                                               arguments.push_back(render_mount_spec(mount));
                                                           }
                                                           for (const auto& file : registry_files)
                                                           {
                                                               arguments.emplace_back("--reg-file");
                                                               arguments.push_back(file.string());
                                                           }
                                                           return arguments;
                                                       }};

            emulator_callbacks callbacks{};
            callbacks.on_stdout = [](const std::string_view data) {
                (void)fwrite(data.data(), 1, data.size(), stdout);
                fflush(stdout);
            };

#ifdef _WIN32
            auto emulator = whp::create_x86_64_emulator(vcpu_count);
#elif defined(__ANDROID__) && defined(__aarch64__)
            auto emulator = create_x86_64_emulator(backend_type::fex);
#elif defined(__ANDROID__)
            auto emulator = create_x86_64_emulator(backend_type::unicorn);
#else
            auto emulator = kvm::create_x86_64_emulator();
#endif

            auto win_emu_ptr = resume_file.empty() ? std::make_unique<windows_emulator>(std::move(emulator), std::move(app_settings),
                                                                                        settings, std::move(callbacks),
                                                                                        emulator_interfaces{.processes = &process_manager})
                                                   : std::make_unique<windows_emulator>(std::move(emulator), settings, std::move(callbacks),
                                                                                        emulator_interfaces{.processes = &process_manager});
            auto& win_emu = *win_emu_ptr;
            win_emu.log.disable_output(true);

            const auto configuration = describe_configuration(settings, registry_files);
            if (resume_file.empty())
            {
                for (const auto& file : registry_files)
                {
                    import_registry_file(win_emu.registry, file);
                }
            }
            else
            {
                // The snapshot carries the registry values the files were imported into.
                snapshot::load_emulator_snapshot(win_emu, resume_file, configuration);
            }

            std::unique_ptr<emulator_process_target> managed_target{};
            if (managed_connection)
            {
                win_emu.setup_process_if_necessary();
                install_inherited_pipes(win_emu, managed_connection->request(), managed_connection->take_inherited_pipes());
                managed_target = std::make_unique<emulator_process_target>(win_emu);
                if (!managed_connection->wait_for_resume(*managed_target))
                {
                    throw std::runtime_error("Acknowledging managed process startup failed");
                }
            }
            const auto managed_process_guard = utils::finally([&] {
                if (managed_connection)
                {
                    managed_connection->disconnect();
                }
            });

            snapshot::controller snapshots{win_emu, configuration, snapshot_directory, [](const snapshot::snapshot_outcome& outcome) {
                                               if (outcome.error.empty())
                                               {
                                                   fprintf(stderr, "Saved the process to %s\n", outcome.path.string().c_str());
                                               }
                                               else
                                               {
                                                   fprintf(stderr, "Saving the process failed, it keeps running: %s\n",
                                                           outcome.error.c_str());
                                               }
                                           }};
            win_emu.ui().set_snapshot_request_sink([&] { snapshots.request(); });
            const auto snapshot_sink_guard = utils::finally([&] { win_emu.ui().set_snapshot_request_sink({}); });

            std::atomic_uint32_t signals_received{0};
            utils::interupt_handler interrupt_guard{[&] {
                if (signals_received++ >= 2)
                {
                    _Exit(1);
                }
            }};

            // A signal handler cannot take the locks stopping the process needs, so a thread watching the interrupts does.
            std::mutex signal_mutex{};
            std::condition_variable signal_changed{};
            bool watching = true;
            std::thread signal_watcher([&] {
                std::unique_lock lock(signal_mutex);
                while (watching)
                {
                    signal_changed.wait_for(lock, std::chrono::milliseconds(50));
                    if (signals_received > 0)
                    {
                        snapshots.stop();
                        return;
                    }
                }
            });

            const auto signal_watcher_guard = utils::finally([&] {
                {
                    const std::scoped_lock lock(signal_mutex);
                    watching = false;
                }
                signal_changed.notify_all();
                signal_watcher.join();
            });

            if (snapshots.run())
            {
                return 0;
            }

            const auto exit_status = win_emu.process.exit_status;
            if (!exit_status.has_value())
            {
                return 1;
            }

            if (managed_connection && !managed_connection->notify_exit(static_cast<uint64_t>(static_cast<uint32_t>(*exit_status))))
            {
                throw std::runtime_error("Reporting managed process exit failed");
            }

            return static_cast<int>(*exit_status);
        }

        int run_main(int argc, char** argv)
        {
            CLI::App app{"Sogen Sandbox"};

            // On Windows this resolves the UTF-8 arguments from the wide command line.
            argv = app.ensure_utf8(argv);

            // Map a guest Windows path to a host path (repeatable, two values each), mirroring
            // the analyzer's -p option. Parsed before the first positional (the application).
            std::vector<std::pair<std::string, std::string>> path_mappings{};
            app.add_option("-p,--path", path_mappings, "Map a Windows path to a host path, same as --mount DST:SRC")
                ->type_name("SRC DST")
                ->allow_extra_args(false);
            std::vector<std::string> mount_specs{};
            app.add_option("--mount", mount_specs, "Mount a host path into the guest file system (MODE is ro or rw)")
                ->type_name("HOST:GUEST[:MODE]")
                ->allow_extra_args(false);
            std::string root_mode{to_string(default_root_mode)};
            app.add_option("--root-mode", root_mode, "Access mode of the emulation root drives")
                ->capture_default_str()
                ->check(CLI::IsMember({"ro", "rw"}));
            std::filesystem::path emulation_root{};
            std::filesystem::path registry_directory{};
            std::vector<std::filesystem::path> registry_files{};
            std::vector<std::pair<std::string, std::string>> environment{};
            uint16_t managed_process_port{};
            std::string managed_process_token{};
            app.add_option("-e,--emulation", emulation_root, "Set emulation root path");
            app.add_option("-r,--registry", registry_directory, "Set registry path");
            app.add_option("--reg-file", registry_files, "Import registry values from a .reg file")
                ->type_name("FILE")
                ->expected(1)
                ->allow_extra_args(false);
            app.add_option("--env", environment, "Set a guest environment variable")->type_name("NAME VALUE")->allow_extra_args(false);
            std::string network_name{network::to_string(network::network_mode::offline)};
            app.add_option("--network", network_name, "Guest network access: offline (no host network) or live")
                ->capture_default_str()
                ->check(CLI::IsMember({"offline", "live"}));
            bool spoof_gpu_features{};
            app.add_flag("--spoof-gpu-features", spoof_gpu_features,
                         "Report Vulkan features DXVK requires even if the GPU lacks them (e.g. Mali)");
            std::filesystem::path snapshot_directory{"snapshots"};
            std::filesystem::path resume_file{};
            app.add_option("--snapshot-dir", snapshot_directory, "Where Ctrl+Alt+Shift+S saves the process")
                ->capture_default_str()
                ->type_name("DIR");
            app.add_option("--resume", resume_file, "Continue a saved process")->type_name("FILE");
            app.add_option("--managed-process-port", managed_process_port)->group("");
            app.add_option("--managed-process-token", managed_process_token)->group("");

            // Stop parsing at the first positional (the application) and forward everything after it to the
            // emulated program.
            app.prefix_command();

            CLI11_PARSE(app, argc, argv);

            try
            {
                auto application = app.remaining();
                std::optional<managed_process_connection> managed_process{};
                if (managed_process_port != 0)
                {
                    if (managed_process_token.empty())
                    {
                        throw std::runtime_error("A managed process token is required");
                    }
                    managed_process.emplace(connect_managed_process(managed_process_port, managed_process_token));
                    const auto& request = managed_process->request();
                    application.clear();
                    application.push_back(request.application);
                    application.insert(application.end(), request.arguments.begin(), request.arguments.end());
                }
                if (application.empty() && resume_file.empty())
                {
                    puts(app.help().c_str());
                    return 1;
                }

                if (!application.empty() && !resume_file.empty())
                {
                    throw std::runtime_error("--resume continues the saved application and takes no other");
                }

                std::vector<mount_spec> mounts{};
                mounts.reserve(path_mappings.size() + mount_specs.size());
                for (const auto& [source, target] : path_mappings)
                {
                    mounts.push_back(mount_spec{
                        .host_path = std::filesystem::absolute(target),
                        .guest_path = windows_path(source),
                    });
                }

                for (const auto& mount : mount_specs)
                {
                    auto spec = parse_mount_spec(mount);
                    spec.host_path = std::filesystem::absolute(spec.host_path);
                    mounts.push_back(std::move(spec));
                }

                const std::vector<std::string_view> views{application.begin(), application.end()};
                return run(views, parse_filesystem_mode(root_mode).value(), std::move(mounts), emulation_root, registry_directory,
                           registry_files, environment, network::parse_network_mode(network_name), spoof_gpu_features, snapshot_directory,
                           resume_file, managed_process ? &*managed_process : nullptr);
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "%s\n", e.what());
            }
            catch (...)
            {
                fprintf(stderr, "An unknown exception occurred\n");
            }

            return 1;
        }
    }
}

int main(int argc, char** argv)
{
    return sogen::sandbox::run_main(argc, argv);
}
