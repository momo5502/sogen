#include <windows_emulator.hpp>
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

        int run(const std::span<const std::string_view> args)
        {
            application_settings app_settings{
                .application = std::u8string(args[0].begin(), args[0].end()),
                .arguments = parse_arguments(args),
            };

#ifdef _WIN32
            // One vCPU per host core; WHP supports at most 64 per partition. EMULATOR_VCPU_COUNT overrides.
            auto vcpu_count = std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
            if (const char* env = std::getenv("EMULATOR_VCPU_COUNT"); env != nullptr && env[0] != '\0')
            {
                vcpu_count = std::clamp(static_cast<uint32_t>(std::strtoul(env, nullptr, 10)), 1u, 64u);
            }
#else
            // The KVM backend does not support multiple vCPUs yet.
            constexpr uint32_t vcpu_count = 1;
#endif

            emulator_settings settings{
                .vcpu_count = vcpu_count,
                .registry_directory = get_current_binary_dir() / "registry",
            };

            // TODO: expose this as a proper command-line option; for now it is taken from the environment.
            if (const char* root = std::getenv("EMULATOR_ROOT"); root != nullptr && root[0] != '\0')
            {
                settings.emulation_root = root;
            }

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

            windows_emulator win_emu{std::move(emulator), std::move(app_settings), settings, std::move(callbacks)};
            win_emu.log.disable_output(true);

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

            return static_cast<int>(*exit_status);
        }

        int run_main(int argc, char** argv)
        {
            CLI::App app{"Sogen Sandbox"};

            // On Windows this resolves the UTF-8 arguments from the wide command line.
            argv = app.ensure_utf8(argv);

            // Stop parsing at the first positional (the application) and forward everything after it to the
            // emulated program.
            app.prefix_command();

            CLI11_PARSE(app, argc, argv);

            try
            {
                const auto application = app.remaining();
                if (application.empty())
                {
                    puts(app.help().c_str());
                    return 1;
                }

                const std::vector<std::string_view> views{application.begin(), application.end()};
                return run(views);
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
