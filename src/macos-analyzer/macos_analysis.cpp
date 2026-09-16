#include "std_include.hpp"

#include "macos_analysis.hpp"

#include <platform/ui_backend.hpp>
#include <screenshot_ui_backend.hpp>

#include <backend_selection.hpp>
#include <logger.hpp>
#include <macos_launch_target.hpp>
#include <macos_memory_report.hpp>

namespace sogen
{
    void macos_module_index::add(std::string name, const uint64_t start, const uint64_t size)
    {
        if (size == 0)
        {
            return;
        }

        this->entries_[start] = entry{.end = start + size, .name = std::move(name)};
    }

    std::string macos_module_index::find(const uint64_t address) const
    {
        auto it = this->entries_.upper_bound(address);
        if (it == this->entries_.begin())
        {
            return "<N/A>";
        }

        --it;

        if (address >= it->second.end)
        {
            return "<N/A>";
        }

        return it->second.name;
    }

    void macos_analysis_context::emit(const macos_analysis_event& event) const
    {
        this->reporter->report(event);
    }

    macos_execution_context macos_analysis_context::execution() const
    {
        macos_execution_context execution{};
        execution.pc = this->emu->emu().read_instruction_pointer();
        execution.module = this->modules.find(execution.pc);
        execution.thread_id = this->emu->process.active_thread != nullptr ? this->emu->process.active_thread->thread_id : 0;
        return execution;
    }

    std::string macos_permission_string(const memory_permission permission)
    {
        std::string result{};
        result.push_back((permission & memory_permission::read) != memory_permission::none ? 'r' : '-');
        result.push_back((permission & memory_permission::write) != memory_permission::none ? 'w' : '-');
        result.push_back((permission & memory_permission::exec) != memory_permission::none ? 'x' : '-');
        return result;
    }

    void register_macos_callbacks(macos_analysis_context& c)
    {
        auto& callbacks = c.emu->callbacks;

        callbacks.on_stdout = [&c](const std::string_view data) {
            c.emit(macos_stdout_chunk_event{.data = std::string(data), .is_stderr = false});
        };

        callbacks.on_stderr = [&c](const std::string_view data) {
            c.emit(macos_stdout_chunk_event{.data = std::string(data), .is_stderr = true});
        };

        callbacks.on_syscall = [&c](const uint64_t id, const std::string_view name) {
            c.suppress_details = c.options->skip_syscalls || c.options->ignored_syscalls.contains(name);

            if (!c.suppress_details)
            {
                macos_syscall_event event{};
                event.call_count = ++c.call_count;
                event.syscall_id = id;
                event.syscall_name = std::string(name);
                event.execution = c.execution();
                c.emit(std::move(event));
            }

            return instruction_hook_continuation::run_instruction;
        };

        callbacks.on_mach_trap = [&c](const uint32_t index, const std::string_view name) {
            c.suppress_details = c.options->skip_syscalls || c.options->ignored_syscalls.contains(name);

            if (!c.suppress_details)
            {
                macos_syscall_event event{};
                event.call_count = ++c.call_count;
                event.syscall_id = index;
                event.syscall_name = std::string(name);
                event.is_mach_trap = true;
                event.execution = c.execution();
                c.emit(std::move(event));
            }

            return instruction_hook_continuation::run_instruction;
        };

        callbacks.on_trace_detail = [&c](const macos_trace_detail& detail) {
            if (!c.suppress_details)
            {
                c.emit(macos_trace_detail_event{.label = detail.label, .value = detail.value});
            }
        };

        callbacks.on_syscall_error = [&c](const std::string_view name, const int64_t error, const std::string_view error_name) {
            if (!c.suppress_details)
            {
                c.emit(macos_syscall_error_event{.syscall_name = std::string(name), .error = error, .error_name = std::string(error_name)});
            }
        };

        callbacks.on_generic_access = [&c](const std::string_view type, const std::string_view name) {
            c.emit(macos_generic_access_event{.type = std::string(type), .name = std::string(name)});
        };

        callbacks.on_generic_activity = [&c](const std::string_view details) {
            c.emit(macos_generic_activity_event{.details = std::string(details)});
        };

        callbacks.on_suspicious_activity = [&c](const std::string_view details) {
            c.emit(macos_suspicious_activity_event{.details = std::string(details), .execution = c.execution()});
        };

        callbacks.on_memory_allocate = [&c](const uint64_t address, const uint64_t length, const memory_permission permissions,
                                            const bool commit) {
            c.emit(macos_memory_allocate_event{
                .address = address, .length = length, .permissions = macos_permission_string(permissions), .commit = commit});
        };

        callbacks.on_memory_protect = [&c](const uint64_t address, const uint64_t length, const memory_permission permissions) {
            c.emit(macos_memory_protect_event{.address = address, .length = length, .permissions = macos_permission_string(permissions)});
        };

        callbacks.on_memory_release = [&c](const uint64_t address, const uint64_t length) {
            c.emit(macos_memory_release_event{.address = address, .length = length});
        };

        (void)callbacks.on_module_load.add([&c](const macos_mapped_module& mod) {
            c.modules.add(mod.name, mod.image_start, mod.size_of_image);
            c.emit(macos_module_load_event{.path = mod.path.string(), .image_base = mod.image_base, .image_size = mod.size_of_image});
        });

        callbacks.on_dyld_image = [&c](const std::string_view path, const uint64_t image_base, const uint64_t image_size) {
            c.emit(macos_dyld_image_event{.path = std::string(path), .image_base = image_base, .image_size = image_size});
        };

        callbacks.on_thread_create = [&c](const uint64_t thread_id, const uint64_t start_address, const uint64_t argument) {
            c.emit(macos_thread_create_event{.thread_id = thread_id, .start_address = start_address, .argument = argument});
        };

        callbacks.on_thread_terminated = [&c](const uint64_t thread_id) { c.emit(macos_thread_terminated_event{.thread_id = thread_id}); };

        callbacks.on_mach_port = [&c](const uint32_t port_name, const std::string_view right, const std::string_view description) {
            c.emit(macos_mach_port_event{.port_name = port_name, .right = std::string(right), .description = std::string(description)});
        };

        callbacks.on_mach_message = [&c](const uint32_t remote_port, const uint32_t message_id, const std::string_view subsystem) {
            c.emit(macos_mach_message_event{.remote_port = remote_port, .message_id = message_id, .subsystem = std::string(subsystem)});
        };

        callbacks.on_process_exit = [&c](const int status) { c.emit(macos_process_exit_event{.exit_status = status}); };

        callbacks.on_cpu_exception = [&c](const uint64_t address, const int index, const std::string_view description) {
            c.emit(macos_cpu_exception_event{.address = address, .exception_index = index, .description = std::string(description)});
        };
    }

    // Reads through the emulator's own accessor so a lazily paged cache region is faulted in the way
    // the guest would fault it, rather than reported absent.
    void dump_guest_memory(macos_emulator& emu, const uint64_t address, const size_t length)
    {
        std::vector<uint8_t> bytes(length);
        if (!emu.emu().try_read_memory(address, bytes.data(), bytes.size()))
        {
            printf("memory 0x%" PRIx64 "+0x%zx: unreadable\n", address, length);
            return;
        }

        for (size_t offset = 0; offset < bytes.size(); offset += 16)
        {
            const auto row = std::min<size_t>(16, bytes.size() - offset);
            printf("0x%" PRIx64 ": ", address + offset);

            for (size_t i = 0; i < row; ++i)
            {
                printf("%02x ", bytes[offset + i]);
            }

            for (size_t i = row; i < 16; ++i)
            {
                printf("   ");
            }

            printf(" |");
            for (size_t i = 0; i < row; ++i)
            {
                const auto c = bytes[offset + i];
                putchar(c >= 0x20 && c < 0x7f ? c : '.');
            }
            printf("|\n");
        }

        for (size_t offset = 0; offset + 8 <= bytes.size(); offset += 8)
        {
            uint64_t word = 0;
            memcpy(&word, bytes.data() + offset, sizeof(word));
            printf("0x%" PRIx64 ": qword 0x%016" PRIx64 "\n", address + offset, word);
        }
    }

    int run_macos_analysis(const macos_analysis_options& options, logger& log)
    {
        auto backend = create_arm64_emulator_from_environment();
        const auto backend_name = backend->get_name();

        macos_emulator emu(std::move(backend), options.emulation_root);

        emu.force_lazy_cache_paging = options.lazy_cache_paging;

        screenshot_ui_backend* screenshot = nullptr;
        if (options.gui || options.interactive || !options.screenshot.empty())
        {
            if (options.interactive)
            {
                // The same backend the Windows and Linux front-ends always use, rather than a mechanism
                // of its own: on a host that is SDL, and an SDL window is what reports that input can
                // still arrive, which is what keeps an application idling on its run loop alive. The
                // raise is asked for here and nowhere else: an interactive run is the only one whose
                // whole point is to be clicked, and a macOS host gives pointer events to no application
                // but the active one.
                emu.set_ui_backend(create_default_ui_backend(ui_backend_options{.raise_new_windows = true}));

                emu.ui.set_input_observer([&emu](const ui_event& event, bool /*delivered*/) {
                    if (event.message == WM_CLOSE)
                    {
                        emu.stop();
                    }
                });

                puts("interactive: the guest's windows open on this host; close one or press Ctrl-C to end the run");
                fflush(stdout);
            }
            else
            {
                auto ui = create_screenshot_ui_backend();
                screenshot = static_cast<screenshot_ui_backend*>(ui.get());
                screenshot->set_desktop_size(options.desktop_width, options.desktop_height);
                emu.set_ui_backend(std::move(ui));
            }

            emu.ui.enabled = true;
            emu.ui.desktop_width = options.desktop_width;
            emu.ui.desktop_height = options.desktop_height;
        }

        emu.trace.decode_arguments = !options.skip_arguments;
        emu.trace.string_limit = options.string_limit;
        emu.trace.buffer_preview_limit = options.buffer_preview_limit;

        // The emulator's own logger is separate from the analyzer's; only the analyzer's feeds the
        // reporter. --verbose is what unmutes the emulator's internal logging.
        if (!options.verbose)
        {
            emu.log.disable_output(true);
        }

        const auto reporter = create_macos_console_reporter(
            log, macos_console_reporter_settings{
                     .silent = options.silent, .concise = options.concise, .prepend_call_count = options.prepend_call_count});

        macos_analysis_context context{.emu = &emu, .reporter = reporter.get(), .options = &options};
        register_macos_callbacks(context);

        context.emit(macos_run_started_event{.backend_name = std::string(backend_name), .application = options.executable.string()});

        // Two ways to name what runs, and they need different handling. A path that resolves inside the
        // emulation root is already a guest path -- that is how a root is meant to be used, and putting it
        // through the host-side resolver would look for it on the analyst's own filesystem and not find
        // it. Anything else is a host path: a .app whose Info.plist names the executable, a .dmg that is
        // not runnable but has a useful answer, or a loose binary.
        const auto inside_root = !options.emulation_root.empty() &&
                                 std::filesystem::exists(options.emulation_root / options.executable.relative_path()) &&
                                 !std::filesystem::exists(options.executable);

        std::string guest_executable = options.executable.generic_string();
        auto argv = options.argv;

        if (!inside_root)
        {
            const auto launch = resolve_macos_launch_target(options.executable);

            if (!launch.runnable())
            {
                context.emit(macos_run_failed_event{.pc = 0, .message = launch.diagnostic});
                reporter->flush();
                return 1;
            }

            // Maps the bundle into the guest read-only, so the app can reach its own resources.
            apply_macos_launch_target(launch, emu.file_sys);
            guest_executable = launch.guest_executable;
            emu.process.current_working_directory = launch.working_directory;

            if (!launch.bundle_identifier.empty())
            {
                emu.log.info("Bundle: %s (%s)\n", launch.bundle_identifier.c_str(), guest_executable.c_str());
            }
        }

        if (!argv.empty())
        {
            // LaunchServices passes the guest path of the inner executable as argv[0], not the bundle
            // directory the user typed. Measured against a real launch.
            argv.front() = guest_executable;
        }

        try
        {
            if (!emu.load_executable(guest_executable, argv, options.envp))
            {
                context.emit(macos_run_failed_event{.pc = 0, .message = emu.last_stop_detail()});
                reporter->flush();
                return 1;
            }

            emu.start(static_cast<size_t>(options.max_instructions));
        }
        catch (const std::exception& e)
        {
            context.emit(macos_run_failed_event{.pc = emu.emu().read_instruction_pointer(), .message = e.what()});
            reporter->flush();

            for (const auto& [address, length] : options.memory_dumps)
            {
                dump_guest_memory(emu, address, length);
            }

            return 1;
        }

        // A launch that dyld refused looks like an ordinary abort from outside, because dyld's halt path
        // writes to no descriptor. Its own message is the only thing that says what went wrong.
        const auto dyld_error = emu.dyld_error_message();
        if (!dyld_error.empty())
        {
            emu.log.error("dyld: %s\n", dyld_error.c_str());
        }

        const auto exit_status = emu.process.exit_status;

        macos_run_finished_event finished{};
        finished.success = exit_status.value_or(-1) == 0;
        finished.exit_status = exit_status;
        finished.instructions = emu.get_executed_instructions();
        finished.stop_detail = emu.last_stop_detail();
        context.emit(std::move(finished));

        reporter->flush();

        for (const auto& [address, length] : options.memory_dumps)
        {
            dump_guest_memory(emu, address, length);
        }

        if (screenshot != nullptr)
        {
            if (screenshot->write(options.screenshot))
            {
                printf("screenshot: %s (%dx%d, %zu window%s, %zu present%s)\n", options.screenshot.string().c_str(), options.desktop_width,
                       options.desktop_height, screenshot->windows().size(), screenshot->windows().size() == 1 ? "" : "s",
                       emu.ui.present_count(), emu.ui.present_count() == 1 ? "" : "s");
            }
            else
            {
                fprintf(stderr, "Error: could not write %s\n", options.screenshot.string().c_str());
            }
        }

        if (options.memory_report)
        {
            printf("%s\n", format_macos_memory_report(collect_macos_memory_report(emu.memory)).c_str());
        }

        return exit_status.value_or(1);
    }
}
