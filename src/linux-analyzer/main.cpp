#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <string>
#include <vector>
#include <filesystem>

#include <linux_emulator.hpp>
#include <linux_x64_gdb_stub_handler.hpp>
#include <gdb_stub.hpp>
#include <network/address.hpp>
#include <backend_selection.hpp>

#include <CLI/CLI.hpp>

#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
#include <linux_event_handler.hpp>
#include <utils/finally.hpp>
#endif

namespace sogen
{
    namespace
    {
        struct cli_options
        {
            std::filesystem::path emulation_root{};
            std::filesystem::path executable{};
            std::vector<std::string> argv{};
            std::vector<std::string> envp{};
            bool verbose{false};
            bool use_gdb{false};
            bool pause_before_start{false};
        };

    }

    int run_main(int argc, char** argv)
    {
        CLI::App app{"Sogen Linux Analyzer"};

        // On Windows this resolves the UTF-8 arguments from the wide command line; elsewhere it is a no-op.
        argv = app.ensure_utf8(argv);

        // Stop parsing at the first positional (the executable) and forward everything after it to the emulated program.
        app.prefix_command();

        cli_options opts{};
        opts.envp = {"PATH=/usr/bin:/bin", "HOME=/root", "TERM=xterm"};

        app.add_option("--root", opts.emulation_root, "Set emulation root directory");
        app.add_flag("--verbose", opts.verbose, "Enable verbose logging");
        app.add_flag("-d,--gdb", opts.use_gdb, "Wait for GDB connection on 127.0.0.1:28960");
        app.add_flag("--break-start", opts.pause_before_start, "Pause before executing the first instruction");

        CLI11_PARSE(app, argc, argv);

        const auto application = app.remaining();
        if (application.empty())
        {
            fprintf(stderr, "Error: No executable specified\n");
            fputs(app.help().c_str(), stderr);
            return 1;
        }

        opts.executable = application.front();
        opts.argv.assign(application.begin(), application.end());

        if (!std::filesystem::exists(opts.executable))
        {
            fprintf(stderr, "Error: Executable not found: %s\n", opts.executable.string().c_str());
            return 1;
        }

        try
        {
            auto emu = create_x86_64_emulator();

            linux_emulator linux_emu(std::move(emu), opts.emulation_root, opts.executable, opts.argv, opts.envp);

            if (!opts.verbose)
            {
                linux_emu.log.disable_output(true);
            }

#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
            std::optional<int> exit_status{};
            const auto exit_handler = utils::finally([&] { linux_debugger::handle_exit(linux_emu, exit_status); });

            linux_debugger::event_context ec{.linux_emu = linux_emu};

            linux_emu.on_periodic_event = [&ec] { linux_debugger::handle_events(ec); };
#endif

            if (opts.use_gdb)
            {
                const auto* address = "127.0.0.1:28960";
                printf("Waiting for GDB connection on %s...\n", address);

                linux_x64_gdb_stub_handler handler{linux_emu};
                gdb_stub::run_gdb_stub(network::address{address, AF_INET}, handler);
            }
            else
            {
#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
                if (opts.pause_before_start)
                {
                    linux_debugger::event_context pause_ec{.linux_emu = linux_emu};
                    linux_debugger::pause_before_start(pause_ec);
                }
#endif
                linux_emu.start();
            }

#if defined(OS_EMSCRIPTEN) && !defined(SOGEN_EMSCRIPTEN_SUPPORT_NODEJS)
            exit_status = linux_emu.process.exit_status;
#endif

            const auto final_exit_status = linux_emu.process.exit_status.value_or(-1);
            const auto instructions = linux_emu.get_executed_instructions();

            printf("\n--- Emulation finished ---\n");
            printf("Exit status: %d\n", final_exit_status);
            printf("Instructions executed: %" PRIu64 "\n", instructions);

            return final_exit_status;
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "Fatal error: %s\n", e.what());
            return 1;
        }
    }

    int linux_main(const int argc, char** argv)
    {
        return run_main(argc, argv);
    }
}
