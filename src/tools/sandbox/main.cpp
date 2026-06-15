#include <windows_emulator.hpp>
#include <whp_x86_64_emulator.hpp>

#include <utils/interupt_handler.hpp>
#include <utils/win.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sogen::sandbox
{
    namespace
    {
        std::filesystem::path get_current_binary_dir()
        {
            std::array<wchar_t, MAX_PATH> buffer{};

            const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length == buffer.size())
            {
                throw std::runtime_error("Resolving module file name failed");
            }

            return std::filesystem::path(buffer.data()).parent_path();
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

        void print_help()
        {
            printf("Usage: sandbox <application> [args...]\n");
        }

        int run(const std::span<const std::string_view> args)
        {
            application_settings app_settings{
                .application = std::u8string(args[0].begin(), args[0].end()),
                .arguments = parse_arguments(args),
            };

            emulator_settings settings{
                .registry_directory = get_current_binary_dir() / "registry",
            };

            emulator_callbacks callbacks{};
            callbacks.on_stdout = [](const std::string_view data) {
                (void)fwrite(data.data(), 1, data.size(), stdout);
                fflush(stdout);
            };

            windows_emulator win_emu{whp::create_x86_64_emulator(), std::move(app_settings), settings, std::move(callbacks)};
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

        int run_main(const int argc, wchar_t** wargv)
        {
            try
            {
                std::vector<std::string> args{};
                for (int i = 1; i < argc; ++i)
                {
                    args.emplace_back(w_to_u8(wargv[i]));
                }

                std::vector<std::string_view> views{};
                views.reserve(args.size());
                for (const auto& str : args)
                {
                    views.push_back(str);
                }

                if (args.empty())
                {
                    print_help();
                    return 1;
                }

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

int wmain(const int argc, wchar_t** argv)
{
    return sogen::sandbox::run_main(argc, argv);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, PSTR, int)
{
    return sogen::sandbox::run_main(__argc, __wargv);
}
