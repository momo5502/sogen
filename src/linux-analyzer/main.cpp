#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

#include <linux_emulator.hpp>
#include <linux_x64_gdb_stub_handler.hpp>
#include <gdb_stub.hpp>
#include <network/address.hpp>
#include <backend_selection.hpp>

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
#include <linux_event_handler.hpp>
#include <utils/finally.hpp>
#endif

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
    };

    void print_usage(const char* program)
    {
        fprintf(stderr, "Usage: %s [options] <executable> [args...]\n", program);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  --root <path>    Set emulation root directory\n");
        fprintf(stderr, "  --verbose        Enable verbose logging\n");
        fprintf(stderr, "  --gdb, -d        Wait for GDB connection on 127.0.0.1:28960\n");
        fprintf(stderr, "  --help           Show this help message\n");
    }

    cli_options parse_args(const int argc, char* argv[])
    {
        cli_options opts{};

        // Default environment
        opts.envp.emplace_back("PATH=/usr/bin:/bin");
        opts.envp.emplace_back("HOME=/root");
        opts.envp.emplace_back("TERM=xterm");

        int i = 1;
        while (i < argc)
        {
            const std::string arg = argv[i];

            if (arg == "--root" && i + 1 < argc)
            {
                opts.emulation_root = argv[++i];
            }
            else if (arg == "--verbose")
            {
                opts.verbose = true;
            }
            else if (arg == "--gdb" || arg == "-d")
            {
                opts.use_gdb = true;
            }
            else if (arg == "--help" || arg == "-h")
            {
                print_usage(argv[0]);
                exit(0);
            }
            else if (arg[0] == '-')
            {
                fprintf(stderr, "Unknown option: %s\n", arg.c_str());
                print_usage(argv[0]);
                exit(1);
            }
            else
            {
                // First non-option argument is the executable
                opts.executable = arg;
                opts.argv.push_back(arg);

                // Remaining arguments are passed to the emulated program
                for (int j = i + 1; j < argc; ++j)
                {
                    opts.argv.emplace_back(argv[j]);
                }
                break;
            }

            ++i;
        }

        return opts;
    }
}

int main(const int argc, char* argv[])
{
    auto opts = parse_args(argc, argv);

    if (opts.executable.empty())
    {
        fprintf(stderr, "Error: No executable specified\n");
        print_usage(argv[0]);
        return 1;
    }

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

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
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
            linux_emu.start();
        }

#if defined(OS_EMSCRIPTEN) && !defined(MOMO_EMSCRIPTEN_SUPPORT_NODEJS)
        exit_status = linux_emu.process.exit_status;
#endif

        const auto final_exit_status = linux_emu.process.exit_status.value_or(-1);
        const auto instructions = linux_emu.get_executed_instructions();

        printf("\n--- Emulation finished ---\n");
        printf("Exit status: %d\n", final_exit_status);
        printf("Instructions executed: %llu\n", static_cast<unsigned long long>(instructions));

        return final_exit_status;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
}
