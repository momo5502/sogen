#include <windows_emulator.hpp>
#include <out_of_process_process_manager.hpp>
#include <emulator_process_target.hpp>
#include <registry/registry_file.hpp>
#ifdef _WIN32
#include <whp_x86_64_emulator.hpp>
#include <utils/win.hpp>
#else
#include <kvm_x86_64_emulator.hpp>
#endif

#include <utils/interupt_handler.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

        std::filesystem::path get_sandbox_executable()
        {
#ifdef _WIN32
            return get_current_binary_dir() / "sandbox.exe";
#else
            return get_current_binary_dir() / "sandbox";
#endif
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

        int run(const std::span<const std::string_view> args, std::unordered_map<windows_path, std::filesystem::path> path_mappings,
                const std::filesystem::path& emulation_root, const std::filesystem::path& registry_directory,
                const std::vector<std::filesystem::path>& registry_files, managed_process_connection* managed_connection)
        {
            application_settings app_settings{
                .application = std::u8string(args[0].begin(), args[0].end()),
                .arguments = parse_arguments(args),
            };
            if (managed_connection)
            {
                const auto& request = managed_connection->request();
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
            // One vCPU per host core; WHP supports at most 64 per partition. EMULATOR_VCPU_COUNT overrides.
            auto vcpu_count = std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
            if (const char* env = std::getenv("EMULATOR_VCPU_COUNT"); env != nullptr && env[0] != '\0')
            {
                vcpu_count = std::clamp(static_cast<uint32_t>(std::strtoul(env, nullptr, 10)), 1u, 64u);
            }
#endif

            emulator_settings settings{
                .registry_directory = registry_directory.empty() ? get_current_binary_dir() / "registry" : registry_directory,
            };

            if (!emulation_root.empty())
            {
                settings.emulation_root = emulation_root;
            }
            else if (const char* root = std::getenv("EMULATOR_ROOT"); root != nullptr && root[0] != '\0')
            {
                settings.emulation_root = root;
            }

            settings.path_mappings = std::move(path_mappings);

            out_of_process_process_manager process_manager{get_sandbox_executable(), [&](const auto port, const auto& token) {
                                                               std::vector<std::string> arguments{"--managed-process-port",
                                                                                                  std::to_string(port),
                                                                                                  "--managed-process-token",
                                                                                                  token,
                                                                                                  "--registry",
                                                                                                  settings.registry_directory.string()};
                                                               if (!settings.emulation_root.empty())
                                                               {
                                                                   arguments.emplace_back("--emulation");
                                                                   arguments.push_back(settings.emulation_root.string());
                                                               }
                                                               for (const auto& [source, target] : settings.path_mappings)
                                                               {
                                                                   arguments.emplace_back("--path");
                                                                   arguments.push_back(u16_to_u8(source.u16string()));
                                                                   arguments.push_back(target.string());
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
#else
            auto emulator = kvm::create_x86_64_emulator();
#endif

            windows_emulator win_emu{std::move(emulator), std::move(app_settings), settings, std::move(callbacks),
                                     emulator_interfaces{.processes = &process_manager}};
            for (const auto& file : registry_files)
            {
                import_registry_file(win_emu.registry, file);
            }
            win_emu.log.disable_output(true);

            if (managed_connection)
            {
                win_emu.setup_process_if_necessary();
                emulator_process_target managed_target{win_emu};
                if (!managed_connection->wait_for_resume(managed_target))
                {
                    throw std::runtime_error("Acknowledging managed process startup failed");
                }
            }

            std::atomic_uint32_t signals_received{0};
            utils::interupt_handler interrupt_guard{[&] {
                const auto value = signals_received++;
                if (value >= 2)
                {
                    _Exit(1);
                }

                win_emu.stop();
            }};

            win_emu.start();

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
            app.add_option("-p,--path", path_mappings, "Map a Windows path to a host path")->type_name("SRC DST")->allow_extra_args(false);
            std::filesystem::path emulation_root{};
            std::filesystem::path registry_directory{};
            std::vector<std::filesystem::path> registry_files{};
            uint16_t managed_process_port{};
            std::string managed_process_token{};
            app.add_option("-e,--emulation", emulation_root, "Set emulation root path");
            app.add_option("-r,--registry", registry_directory, "Set registry path");
            app.add_option("--reg-file", registry_files, "Import registry values from a .reg file")
                ->type_name("FILE")
                ->expected(1)
                ->allow_extra_args(false);
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
                if (application.empty())
                {
                    puts(app.help().c_str());
                    return 1;
                }

                std::unordered_map<windows_path, std::filesystem::path> mappings{};
                for (const auto& [source, target] : path_mappings)
                {
                    mappings[windows_path(source)] = std::filesystem::absolute(target);
                }

                const std::vector<std::string_view> views{application.begin(), application.end()};
                return run(views, std::move(mappings), emulation_root, registry_directory, registry_files,
                           managed_process ? &*managed_process : nullptr);
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
