#include "std_include.hpp"

#include "macos_analysis.hpp"

#include <logger.hpp>

#include <CLI/CLI.hpp>

#ifndef _WIN32
#include <csignal>
#endif

namespace sogen
{
    namespace
    {
        void split_and_insert(std::set<std::string, std::less<>>& target, const std::string_view values)
        {
            size_t offset = 0;

            while (offset <= values.size())
            {
                const auto separator = values.find(',', offset);
                const auto end = separator == std::string_view::npos ? values.size() : separator;
                const auto piece = values.substr(offset, end - offset);

                if (!piece.empty())
                {
                    target.emplace(piece);
                }

                if (separator == std::string_view::npos)
                {
                    break;
                }

                offset = separator + 1;
            }
        }

        int run_main(int argc, char** argv)
        {
#ifndef _WIN32
            signal(SIGPIPE, SIG_IGN);
#endif

            CLI::App app{"Sogen macOS Analyzer"};

            argv = app.ensure_utf8(argv);
            app.prefix_command();
            app.footer("Examples:\n"
                       "  analyzer --os=macos ./hello\n"
                       "  analyzer --os=macos -e path/to/root -i getpid,gettimeofday ./hello\n"
                       "  analyzer --os=macos --skip-args --call-count ./hello");

            macos_analysis_options options{};

            std::string os_name{};
            app.add_option("--os", os_name, "Operating system personality (handled by the analyzer front-end)");

            app.add_option("-e,--emulation,--root", options.emulation_root, "Set emulation root path");
            app.add_flag("-s,--silent", options.silent, "Print guest output only");
            app.add_flag("-v,--verbose", options.verbose, "Enable verbose emulator logging");
            app.add_flag("-c,--concise", options.concise, "Concise logging: drop memory and generic activity lines");
            app.add_flag("--skip-syscalls", options.skip_syscalls, "Skip syscall lines and their decoded arguments");
            app.add_flag("--skip-args", options.skip_arguments, "Disable syscall argument decoding");
            app.add_flag("--call-count", options.prepend_call_count, "Prefix syscall lines with a traced-call count");
            app.add_flag("--memory-report", options.memory_report, "Print peak guest and host memory usage");
            app.add_option("--string-limit", options.string_limit, "Maximum guest string length shown in a trace line")
                ->capture_default_str();
            app.add_option("--buffer-limit", options.buffer_preview_limit, "Maximum buffer bytes previewed as hex")->capture_default_str();
            app.add_option("--max-instructions", options.max_instructions, "Stop after this many instructions (0 = unbounded)")
                ->capture_default_str();

            std::vector<std::string> ignored_syscalls{};
            app.add_option("-i,--ignore", ignored_syscalls, "Comma-separated list of syscalls to ignore")->allow_extra_args(false);

            app.add_flag("--gui", options.gui, "Intercept the window path so a GUI app can start up (implied by --screenshot)");
            app.add_option("--screenshot", options.screenshot, "Compose the guest's windows and write them as a PNG when the run ends");
            app.add_flag("--interactive", options.interactive,
                         "Show the guest's windows on the host and deliver clicks and keys to them (implies --gui)");

            app.add_flag("--lazy-cache-paging", options.lazy_cache_paging,
                         "Materialise the shared cache on demand instead of mapping it, as the browser must");

            std::string desktop_size{};
            app.add_option("--desktop-size", desktop_size, "Emulated desktop size as WIDTHxHEIGHT")->type_name("WxH");

            std::vector<std::string> memory_dumps{};
            app.add_option("--dump-memory", memory_dumps, "Dump guest memory as ADDRESS[:LENGTH] once the run ends")
                ->allow_extra_args(false);

            std::vector<std::pair<std::string, std::string>> environment{};
            app.add_option("--env", environment, "Set a guest environment variable")->type_name("NAME VALUE")->allow_extra_args(false);

            CLI11_PARSE(app, argc, argv);

            const auto application = app.remaining();
            if (application.empty())
            {
                fputs("Error: No executable specified\n", stderr);
                fputs(app.help().c_str(), stderr);
                return 1;
            }

            options.executable = application.front();
            options.argv.assign(application.begin(), application.end());

            if (options.envp.empty())
            {
                options.envp = {"PATH=/usr/bin:/bin", "HOME=/var/root", "TMPDIR=/tmp"};
            }

            for (const auto& values : ignored_syscalls)
            {
                split_and_insert(options.ignored_syscalls, values);
            }

            if (options.interactive && !options.screenshot.empty())
            {
                fputs("Error: --interactive and --screenshot are exclusive. The PNG is composed by the headless backend, which the "
                      "live window replaces; capture the window with the host's own screenshot tool instead.\n",
                      stderr);
                return 1;
            }

            if (!desktop_size.empty())
            {
                const auto separator = desktop_size.find_first_of("xX");
                const auto width = std::strtol(desktop_size.substr(0, separator).c_str(), nullptr, 10);
                const auto height =
                    separator == std::string::npos ? 0 : std::strtol(desktop_size.substr(separator + 1).c_str(), nullptr, 10);

                if (width <= 0 || height <= 0)
                {
                    fprintf(stderr, "Error: --desktop-size expects WIDTHxHEIGHT, got '%s'\n", desktop_size.c_str());
                    return 1;
                }

                options.desktop_width = static_cast<int32_t>(width);
                options.desktop_height = static_cast<int32_t>(height);
            }

            for (const auto& request : memory_dumps)
            {
                const auto separator = request.find(':');
                const auto address = std::strtoull(request.substr(0, separator).c_str(), nullptr, 0);
                const auto length = separator == std::string::npos
                                        ? size_t{64}
                                        : static_cast<size_t>(std::strtoull(request.substr(separator + 1).c_str(), nullptr, 0));

                if (address == 0 || length == 0)
                {
                    fprintf(stderr, "Error: --dump-memory expects ADDRESS[:LENGTH], got '%s'\n", request.c_str());
                    return 1;
                }

                options.memory_dumps.emplace_back(address, length);
            }

            for (const auto& [name, value] : environment)
            {
                auto entry = name;
                entry.append("=").append(value);
                options.envp.push_back(std::move(entry));
            }

            // With an emulation root the executable names a path inside the guest, so it has to be
            // resolved through the root before it can be looked for on the host. relative_path() drops
            // the leading separator, which operator/ would otherwise treat as "replace everything".
            const auto host_candidate =
                options.emulation_root.empty() ? options.executable : options.emulation_root / options.executable.relative_path();

            // A host path is accepted as given as well: a .app bundle or a .dmg is usually somewhere on
            // the analyst's disk rather than inside the emulation root, and refusing it here would make
            // the launch resolver's own diagnostic unreachable.
            if (!std::filesystem::exists(host_candidate) && !std::filesystem::exists(options.executable))
            {
                fprintf(stderr, "Error: Executable not found: %s\n", host_candidate.string().c_str());
                return 1;
            }

            logger log{};

            try
            {
                return run_macos_analysis(options, log);
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "Fatal error: %s\n", e.what());
                return 1;
            }
            catch (...)
            {
                fputs("Fatal error: unknown exception\n", stderr);
                return 1;
            }
        }
    }

    int macos_main(const int argc, char** argv)
    {
        return run_main(argc, argv);
    }
}
