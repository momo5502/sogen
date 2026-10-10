#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <macos_emulator.hpp>
#include <backend_selection.hpp>

#include <CLI/CLI.hpp>

namespace sogen
{
    int macos_main(int argc, char** argv)
    {
        CLI::App app{"Sogen macOS Analyzer"};

        argv = app.ensure_utf8(argv);
        app.prefix_command();

        bool verbose{false};
        app.add_flag("--verbose", verbose, "Enable verbose logging");

        CLI11_PARSE(app, argc, argv);

        const auto application = app.remaining();
        if (application.empty())
        {
            fprintf(stderr, "Error: No executable specified\n");
            fputs(app.help().c_str(), stderr);
            return 1;
        }

        const std::filesystem::path executable = application.front();
        if (!std::filesystem::exists(executable))
        {
            fprintf(stderr, "Error: Executable not found: %s\n", executable.string().c_str());
            return 1;
        }

        try
        {
            const std::vector<std::string> guest_argv(application.begin(), application.end());
            macos_emulator macos_emu(create_x86_64_emulator_from_environment(), executable, guest_argv, {});

            if (!verbose)
            {
                macos_emu.log.disable_output(true);
            }

            macos_emu.start();

            const auto exit_status = macos_emu.exit_status.value_or(-1);

            printf("\n--- Emulation finished ---\n");
            printf("Exit status: %d\n", exit_status);

            return exit_status;
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "Fatal error: %s\n", e.what());
            return 1;
        }
    }
}
