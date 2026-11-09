#include "std_include.hpp"
#include "session.hpp"

#include <future>
#include <iterator>

#include "android_process_launcher.hpp"
#include "local_steam_backend.hpp"

#include <backend_selection.hpp>
#include <managed_process_start.hpp>
#include <utils/finally.hpp>
#include <filesystem/document_mount.hpp>
#include <filesystem/layered_provider.hpp>
#include <registry/registry_file.hpp>
#include <registry/registry_state_file.hpp>
#include <snapshot.hpp>
#include <subprocess_process_manager.hpp>
#include <windows_emulator.hpp>

namespace sogen::android
{
    namespace
    {
        // Opening a document costs milliseconds, and games open the same few files over and over.
        constexpr size_t cached_document_descriptors = 64;

        jstring make_java_string(JNIEnv& env, const std::u16string_view text)
        {
            return env.NewString(reinterpret_cast<const jchar*>(text.data()), static_cast<jsize>(text.size()));
        }

        backend_type parse_backend(const std::string_view name)
        {
            static const std::map<std::string_view, backend_type> backends{
                {"unicorn", backend_type::unicorn},
                {"icicle", backend_type::icicle},
                {"fex", backend_type::fex},
            };

            if (name.empty())
            {
#if defined(__aarch64__)
                return backend_type::fex;
#else
                return backend_type::unicorn;
#endif
            }

            const auto it = backends.find(name);
            if (it == backends.end())
            {
                throw std::runtime_error("Unknown backend: " + std::string{name});
            }

            return it->second;
        }

        application_settings create_application_settings(const run_config& config)
        {
            application_settings settings{
                .application = std::u8string(config.application.begin(), config.application.end()),
            };

            for (const auto& argument : config.arguments)
            {
                settings.arguments.push_back(u8_to_u16(argument));
            }

            for (const auto& [name, value] : config.environment)
            {
                settings.environment.insert_or_assign(u8_to_u16(name), u8_to_u16(value));
            }

            if (!config.working_directory.empty())
            {
                settings.working_directory = windows_path(u8_to_u16(config.working_directory));
            }

            return settings;
        }

        emulator_settings create_emulator_settings(const run_config& config)
        {
            // Like the sandbox, games read the TSC and KUSER_SHARED_DATA natively instead of trapping every access.
            return emulator_settings{
                .intercept_rdtsc = false,
                .intercept_kusd = false,
                // A game resumed from a save goes on from it, as its timeouts expect.
                .keep_guest_time_across_snapshots = true,
                .emulation_root = config.emulation_root,
                .root_mode = filesystem_mode::copy_on_write,
                .root_layer = config.root_layer,
                .network = config.live_network ? network::network_mode::live : network::network_mode::offline,
                .spoof_gpu_features = true,
                .max_display_mode = config.max_display_mode,
            };
        }

        // What a saved process depends on outside of itself, which a resume has to find unchanged.
        std::string describe_configuration(const run_config& config)
        {
            std::string configuration = "emulation-root=" + config.emulation_root.string() + "\nroot-layer=" + config.root_layer.string() +
                                        "\nbackend=" + config.backend + "\nnetwork=" + (config.live_network ? "live" : "offline") +
                                        "\nmax-display-mode=" + (config.max_display_mode ? to_string(*config.max_display_mode) : "") +
                                        "\nsteam-app-id=" + std::to_string(config.steam_app_id) +
                                        "\nsteam-id=" + std::to_string(config.steam_id) + "\nsteam-language=" + config.steam_language +
                                        "\nsteam-ownership-ticket=" + config.steam_ownership_ticket_digest + "\n";
            for (const auto& mount : config.directory_mounts)
            {
                configuration += "directory=" + mount.host_directory.string() + ":" + mount.guest_path.string() + ":" +
                                 mount.upper_directory.string() + "\n";
            }
            for (const auto& tree : config.document_trees)
            {
                configuration += "documents=" + tree.uri + ":" + tree.guest_path + ":" + tree.upper_directory.string() + "\n";
            }
            return configuration;
        }

        // The game's library runs every process, so a child only needs to know where to reach its parent.
        std::vector<std::string> create_child_arguments(const uint16_t port, const std::string& token)
        {
            return {"--managed-process-port", std::to_string(port), "--managed-process-token", token};
        }

        managed_process_connection connect_to_parent(const std::vector<std::string>& arguments)
        {
            uint16_t port{};
            std::string token{};
            for (size_t i = 0; i + 1 < arguments.size(); ++i)
            {
                if (arguments[i] == "--managed-process-port")
                {
                    port = static_cast<uint16_t>(std::stoul(arguments[i + 1]));
                }
                else if (arguments[i] == "--managed-process-token")
                {
                    token = arguments[i + 1];
                }
            }

            if (port == 0 || token.empty())
            {
                throw std::runtime_error("A child process needs its parent's port and token");
            }
            return connect_managed_process(port, token);
        }

        application_settings create_child_application_settings(const process_create_request& request)
        {
            application_settings settings{
                .application = std::u8string(request.application.begin(), request.application.end()),
            };
            for (const auto& argument : request.arguments)
            {
                settings.arguments.push_back(u8_to_u16(argument));
            }
            apply_managed_request(settings, request);
            return settings;
        }
    }

    session::session(JavaVM& vm, JNIEnv& env, const jobject listener)
        : listener_(env.NewGlobalRef(listener)),
          dispatcher_(vm)
    {
        auto* listener_class = env.GetObjectClass(listener);
        this->methods_ = {
            .on_output = env.GetMethodID(listener_class, "onOutput", "(Ljava/lang/String;I)V"),
            .on_console_output = env.GetMethodID(listener_class, "onConsoleOutput", "([B)V"),
            .on_window_created = env.GetMethodID(listener_class, "onWindowCreated", "(JIILjava/lang/String;Z)V"),
            .on_window_destroyed = env.GetMethodID(listener_class, "onWindowDestroyed", "(J)V"),
            .on_window_resized = env.GetMethodID(listener_class, "onWindowResized", "(JII)V"),
            .on_window_visibility_changed = env.GetMethodID(listener_class, "onWindowVisibilityChanged", "(JZ)V"),
            .on_window_title_changed = env.GetMethodID(listener_class, "onWindowTitleChanged", "(JLjava/lang/String;)V"),
            .on_cursor_moved = env.GetMethodID(listener_class, "onCursorMoved", "(JII)V"),
            .on_relative_mouse_changed = env.GetMethodID(listener_class, "onRelativeMouseChanged", "(JZ)V"),
            .on_snapshot = env.GetMethodID(listener_class, "onSnapshot", "(Ljava/lang/String;Ljava/lang/String;)V"),
            .on_guest_exited = env.GetMethodID(listener_class, "onGuestExited", "()V"),
            .open_document = env.GetMethodID(listener_class, "openDocument", "(Ljava/lang/String;Ljava/lang/String;)I"),
            .start_child = env.GetMethodID(listener_class, "startChild", "(J[Ljava/lang/String;)V"),
            .kill_child = env.GetMethodID(listener_class, "killChild", "(J)V"),
        };
        env.DeleteLocalRef(listener_class);

        this->terminal_.emplace([this](std::string data) {
            this->dispatcher_.post([this, data = std::move(data)](JNIEnv& env) {
                auto* bytes = env.NewByteArray(static_cast<jsize>(data.size()));
                env.SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte*>(data.data()));
                env.CallVoidMethod(this->listener_, this->methods_.on_console_output, bytes);
                env.DeleteLocalRef(bytes);
            });
        });
    }

    session::~session()
    {
        this->terminal_.reset();
        this->dispatcher_.post([listener = this->listener_](JNIEnv& env) { env.DeleteGlobalRef(listener); });
    }

    int session::run(const run_config& config)
    {
        const auto exit_code = this->run_emulator(config);

        // The caller reports the exit right after this returns, which must not overtake the last output.
        this->dispatcher_.flush();
        return exit_code;
    }

    int session::run_emulator(const run_config& config)
    {
        try
        {
            std::optional<managed_process_connection> parent{};
            if (!config.managed_arguments.empty())
            {
                parent.emplace(connect_to_parent(config.managed_arguments));
            }

            auto ui = std::make_unique<android_ui_backend>(*this, config.vulkan);
            auto* ui_backend = ui.get();

            emulator_callbacks callbacks{};
            callbacks.on_stdout = pseudo_terminal::write_output;

            auto settings = create_emulator_settings(config);

            std::vector<provider_mount> mounts{};
            for (const auto& mount : config.directory_mounts)
            {
                mounts.push_back(provider_mount{
                    .guest_path = mount.guest_path,
                    .provider = create_directory_mount(mount),
                    .mode = filesystem_mode::copy_on_write,
                });
            }

            for (const auto& tree : config.document_trees)
            {
                auto source = std::make_shared<descriptor_document_source>(
                    [this, uri = tree.uri](const std::string& document_id) { return this->open_document(uri, document_id); },
                    cached_document_descriptors);

                mounts.push_back(provider_mount{
                    .guest_path = windows_path{tree.guest_path},
                    .provider =
                        std::make_unique<layered_provider>(std::make_unique<document_tree_provider>(tree.entries, std::move(source)),
                                                           tree.upper_directory, tree.state_directory),
                    .mode = filesystem_mode::copy_on_write,
                });
            }

            // The launcher outlives the process manager: tearing the manager down waits for its children to exit, and the app
            // reports their exits to the launcher.
            std::unique_ptr<android_process_launcher> launcher{};
            std::unique_ptr<subprocess_process_manager> processes{};
            if (!config.sandbox_executable.empty())
            {
                launcher = std::make_unique<android_process_launcher>(
                    [this](const uint64_t request, const std::vector<std::string>& arguments) { this->start_child(request, arguments); },
                    [this](const uint64_t request) { this->kill_child(request); });
                processes = std::make_unique<subprocess_process_manager>(config.sandbox_executable, create_child_arguments,
                                                                         std::make_unique<process_launcher_reference>(*launcher));
            }
            {
                const std::lock_guard lock{this->mutex_};
                this->launcher_ = launcher.get();
                this->processes_ = processes.get();
            }
            const auto processes_guard = utils::finally([&] {
                {
                    const std::lock_guard lock{this->mutex_};
                    this->processes_ = nullptr;
                }
                processes.reset();
                const std::lock_guard lock{this->mutex_};
                this->launcher_ = nullptr;
            });

            // Declared before the emulator, so the guest's windows are gone while this process waits for its children. Any
            // other way out leaves the children to the process manager's destructor.
            bool guest_exited = false;
            const auto children_guard = utils::finally([&] {
                if (processes && guest_exited)
                {
                    processes->wait_for_processes();
                }
            });

            auto emulator = create_x86_64_emulator(parse_backend(config.backend));
            emulator_interfaces interfaces{.ui = std::move(ui), .processes = processes.get(), .mounts = std::move(mounts)};
            if (config.steam_app_id != 0)
            {
                interfaces.steam = create_local_steam_backend(steam_launch{
                    .app_id = config.steam_app_id,
                    .steam_id = config.steam_id,
                    .language = config.steam_language,
                    .ownership_ticket = config.steam_ownership_ticket,
                });
            }
            const bool runs_steam_provider = interfaces.steam != nullptr;
            auto win_emu_ptr = config.resume_file.empty()
                                   ? std::make_unique<windows_emulator>(std::move(emulator),
                                                                        parent ? create_child_application_settings(parent->request())
                                                                               : create_application_settings(config),
                                                                        std::move(settings), std::move(callbacks), std::move(interfaces))
                                   : std::make_unique<windows_emulator>(std::move(emulator), std::move(settings), std::move(callbacks),
                                                                        std::move(interfaces));
            auto& win_emu = *win_emu_ptr;

            win_emu.log.disable_output(true);
            win_emu.log.set_sink([this](const color c, const std::string_view message) {
                if (c != color::gray && c != color::dark_gray)
                {
                    this->output(std::string{message}, static_cast<int32_t>(c));
                }
            });

            win_emu.log.info("Using emulator backend: %s\n", win_emu.emu().get_name().c_str());

            // Restoring re-creates the windows, and their surfaces only arrive through the UI backend.
            {
                const std::lock_guard lock{this->mutex_};
                this->ui_ = ui_backend;
            }

            const auto configuration = describe_configuration(config);
            const auto registry_state =
                config.registry_state.empty() ? nullptr : std::make_unique<registry_state_file>(config.registry_state);
            if (!config.resume_file.empty())
            {
                if (runs_steam_provider)
                {
                    throw std::runtime_error("A suspended Steam game cannot be resumed yet. Start it over.");
                }

                snapshot::load_emulator_snapshot(win_emu, config.resume_file, configuration);
            }
            else
            {
                if (registry_state)
                {
                    registry_state->load(win_emu.registry);
                }
                for (const auto& file : config.registry_files)
                {
                    import_registry_file(win_emu.registry, file);
                }
                if (!config.expanded_registry_files.empty())
                {
                    const auto expand = win_emu.environment_string_expander();
                    for (const auto& file : config.expanded_registry_files)
                    {
                        import_registry_file(win_emu.registry, file, expand);
                    }
                }
            }

            std::unique_ptr<emulator_process_target> parent_target{};
            if (parent && config.resume_file.empty())
            {
                parent_target = start_managed_process(win_emu, *parent);
            }
            const auto parent_guard = utils::finally([&] {
                if (parent)
                {
                    parent->disconnect();
                }
            });

            snapshot::controller snapshots{win_emu, configuration, config.snapshot_directory,
                                           [this](const snapshot::snapshot_outcome& outcome) { this->report_snapshot(outcome); }};

            {
                const std::lock_guard lock{this->mutex_};
                if (this->stop_requested_)
                {
                    this->ui_ = nullptr;
                    return 1;
                }

                this->emulator_ = &win_emu;
                this->snapshots_ = &snapshots;
                this->runs_steam_game_ = runs_steam_provider;
                win_emu.frames().set_limit(this->frame_limit_);
            }

            const bool saved = snapshots.run();

            {
                const std::lock_guard lock{this->mutex_};
                this->emulator_ = nullptr;
                this->snapshots_ = nullptr;
                this->ui_ = nullptr;
            }

            if (saved)
            {
                return 0;
            }

            const auto exit_status = win_emu.process.exit_status;
            if (registry_state && exit_status)
            {
                registry_state->save(win_emu.registry);
            }
            if (parent && exit_status && !parent->notify_exit(static_cast<uint64_t>(static_cast<uint32_t>(*exit_status))))
            {
                throw std::runtime_error("Reporting the exit to the parent process failed");
            }
            guest_exited = exit_status.has_value();
            if (guest_exited)
            {
                this->report_guest_exit();
            }
            return exit_status ? static_cast<int>(*exit_status) : 1;
        }
        catch (const std::exception& e)
        {
            this->output(std::string{"Emulation failed: "} + e.what() + "\n", static_cast<int32_t>(color::red));
        }

        {
            const std::lock_guard lock{this->mutex_};
            this->emulator_ = nullptr;
            this->snapshots_ = nullptr;
            this->ui_ = nullptr;
        }

        return 1;
    }

    void session::stop()
    {
        const std::lock_guard lock{this->mutex_};

        this->stop_requested_ = true;
        if (this->snapshots_)
        {
            this->snapshots_->stop();
        }
        if (this->launcher_)
        {
            this->launcher_->cancel_launches();
        }
        if (this->processes_)
        {
            this->processes_->kill_processes();
        }
    }

    void session::child_started(const uint64_t request, const int32_t process_id)
    {
        const std::lock_guard lock{this->mutex_};
        if (this->launcher_)
        {
            this->launcher_->started(request, process_id);
        }
    }

    void session::child_exited(const uint64_t request, const int32_t exit_status)
    {
        const std::lock_guard lock{this->mutex_};
        if (this->launcher_)
        {
            this->launcher_->exited(request, exit_status);
        }
    }

    void session::start_child(const uint64_t request, const std::vector<std::string>& arguments)
    {
        this->dispatcher_.post([this, request, arguments](JNIEnv& env) {
            auto* string_class = env.FindClass("java/lang/String");
            auto* java_arguments = env.NewObjectArray(static_cast<jsize>(arguments.size()), string_class, nullptr);
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                auto* argument = make_java_string(env, u8_to_u16(arguments[i]));
                env.SetObjectArrayElement(java_arguments, static_cast<jsize>(i), argument);
                env.DeleteLocalRef(argument);
            }
            env.CallVoidMethod(this->listener_, this->methods_.start_child, static_cast<jlong>(request), java_arguments);
            if (env.ExceptionCheck())
            {
                env.ExceptionClear();
            }
            env.DeleteLocalRef(java_arguments);
            env.DeleteLocalRef(string_class);
        });
    }

    void session::kill_child(const uint64_t request)
    {
        this->dispatcher_.post([this, request](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.kill_child, static_cast<jlong>(request));
            if (env.ExceptionCheck())
            {
                env.ExceptionClear();
            }
        });
    }

    bool session::suspend(std::filesystem::path destination)
    {
        const std::lock_guard lock{this->mutex_};
        if (this->snapshots_ && this->runs_steam_game_)
        {
            this->report_snapshot({.path = std::move(destination), .error = "Steam games cannot be suspended yet."});
            return true;
        }

        return this->snapshots_ && this->snapshots_->request(std::move(destination));
    }

    void session::set_frame_limit(const uint32_t frames_per_second)
    {
        const std::lock_guard lock{this->mutex_};
        this->frame_limit_ = frames_per_second;
        if (this->emulator_)
        {
            this->emulator_->frames().set_limit(frames_per_second);
        }
    }

    std::optional<frame_pacer::statistics> session::frame_statistics()
    {
        const std::lock_guard lock{this->mutex_};
        if (!this->emulator_)
        {
            return std::nullopt;
        }
        return this->emulator_->frames().take_statistics();
    }

    void session::report_snapshot(const snapshot::snapshot_outcome& outcome)
    {
        this->dispatcher_.post([this, path = outcome.path.u16string(), error = u8_to_u16(outcome.error)](JNIEnv& env) {
            auto* java_path = make_java_string(env, path);
            auto* java_error = make_java_string(env, error);
            env.CallVoidMethod(this->listener_, this->methods_.on_snapshot, java_path, java_error);
            env.DeleteLocalRef(java_path);
            env.DeleteLocalRef(java_error);
        });
    }

    void session::report_guest_exit()
    {
        this->dispatcher_.post([this](JNIEnv& env) { env.CallVoidMethod(this->listener_, this->methods_.on_guest_exited); });
    }

    unique_fd session::open_document(const std::string& tree, const std::string& document_id)
    {
        std::promise<int> descriptor{};
        auto result = descriptor.get_future();
        this->dispatcher_.post([this, &tree, &document_id, &descriptor](JNIEnv& env) {
            auto* java_tree = make_java_string(env, u8_to_u16(tree));
            auto* java_id = make_java_string(env, u8_to_u16(document_id));
            const auto fd = env.CallIntMethod(this->listener_, this->methods_.open_document, java_tree, java_id);
            if (env.ExceptionCheck())
            {
                env.ExceptionClear();
            }
            env.DeleteLocalRef(java_tree);
            env.DeleteLocalRef(java_id);
            descriptor.set_value(fd);
        });

        return unique_fd{result.get()};
    }

    void session::with_ui(const std::function<void(android_ui_backend&)>& action)
    {
        const std::lock_guard lock{this->mutex_};

        if (this->ui_)
        {
            action(*this->ui_);
        }
    }

    void session::console_input(const std::string_view data) const
    {
        this->terminal_->write_input(data);
    }

    void session::console_resize(const uint16_t columns, const uint16_t rows) const
    {
        this->terminal_->resize(columns, rows);
    }

    void session::on_window_created(const hwnd window, const int32_t width, const int32_t height, const std::u16string_view title,
                                    const bool visible)
    {
        this->dispatcher_.post([this, window, width, height, title = std::u16string{title}, visible](JNIEnv& env) {
            auto* java_title = make_java_string(env, title);
            env.CallVoidMethod(this->listener_, this->methods_.on_window_created, static_cast<jlong>(window), width, height, java_title,
                               static_cast<jboolean>(visible));
            env.DeleteLocalRef(java_title);
        });
    }

    void session::on_window_destroyed(const hwnd window)
    {
        this->dispatcher_.post([this, window](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.on_window_destroyed, static_cast<jlong>(window));
        });
    }

    void session::on_window_resized(const hwnd window, const int32_t width, const int32_t height)
    {
        this->dispatcher_.post([this, window, width, height](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.on_window_resized, static_cast<jlong>(window), width, height);
        });
    }

    void session::on_cursor_moved(const hwnd window, const int32_t x, const int32_t y)
    {
        this->dispatcher_.post([this, window, x, y](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.on_cursor_moved, static_cast<jlong>(window), x, y);
        });
    }

    void session::on_relative_mouse_changed(const hwnd window, const bool relative)
    {
        this->dispatcher_.post([this, window, relative](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.on_relative_mouse_changed, static_cast<jlong>(window),
                               static_cast<jboolean>(relative));
        });
    }

    void session::on_window_visibility_changed(const hwnd window, const bool visible)
    {
        this->dispatcher_.post([this, window, visible](JNIEnv& env) {
            env.CallVoidMethod(this->listener_, this->methods_.on_window_visibility_changed, static_cast<jlong>(window),
                               static_cast<jboolean>(visible));
        });
    }

    void session::on_window_title_changed(const hwnd window, const std::u16string_view title)
    {
        this->dispatcher_.post([this, window, title = std::u16string{title}](JNIEnv& env) {
            auto* java_title = make_java_string(env, title);
            env.CallVoidMethod(this->listener_, this->methods_.on_window_title_changed, static_cast<jlong>(window), java_title);
            env.DeleteLocalRef(java_title);
        });
    }

    void session::output(std::string text, const int32_t color)
    {
        this->dispatcher_.post([this, text = std::move(text), color](JNIEnv& env) {
            auto* java_text = make_java_string(env, u8_to_u16(text));
            env.CallVoidMethod(this->listener_, this->methods_.on_output, java_text, color);
            env.DeleteLocalRef(java_text);
        });
    }
}
