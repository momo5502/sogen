#include "std_include.hpp"
#include "session.hpp"

#include <backend_selection.hpp>
#include <windows_emulator.hpp>

namespace sogen::android
{
    namespace
    {
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
            // Games keep their saves and settings in the emulation root and in their own directory. Like the
            // sandbox, they read the TSC and KUSER_SHARED_DATA natively instead of trapping every access.
            emulator_settings settings{
                .intercept_rdtsc = false,
                .intercept_kusd = false,
                .emulation_root = config.emulation_root,
                .root_mode = filesystem_mode::read_write,
                .network = config.live_network ? network::network_mode::live : network::network_mode::offline,
                .spoof_gpu_features = true,
            };

            for (const auto& [guest, host] : config.path_mappings)
            {
                settings.mounts.push_back(mount_spec{
                    .host_path = host,
                    .guest_path = windows_path(u8_to_u16(guest)),
                    .mode = filesystem_mode::read_write,
                });
            }

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
            .on_window_created = env.GetMethodID(listener_class, "onWindowCreated", "(JIILjava/lang/String;Z)V"),
            .on_window_destroyed = env.GetMethodID(listener_class, "onWindowDestroyed", "(J)V"),
            .on_window_resized = env.GetMethodID(listener_class, "onWindowResized", "(JII)V"),
            .on_window_visibility_changed = env.GetMethodID(listener_class, "onWindowVisibilityChanged", "(JZ)V"),
            .on_window_title_changed = env.GetMethodID(listener_class, "onWindowTitleChanged", "(JLjava/lang/String;)V"),
            .on_cursor_moved = env.GetMethodID(listener_class, "onCursorMoved", "(JII)V"),
        };
        env.DeleteLocalRef(listener_class);
    }

    session::~session()
    {
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
            auto ui = std::make_unique<android_ui_backend>(*this);
            auto* ui_backend = ui.get();

            emulator_callbacks callbacks{};
            callbacks.on_stdout = [this](const std::string_view data) { this->output(std::string{data}, -1); };

            windows_emulator win_emu{create_x86_64_emulator(parse_backend(config.backend)), create_application_settings(config),
                                     create_emulator_settings(config), std::move(callbacks), emulator_interfaces{.ui = std::move(ui)}};

            win_emu.log.disable_output(true);
            win_emu.log.set_sink([this](const color c, const std::string_view message) {
                if (c != color::gray && c != color::dark_gray)
                {
                    this->output(std::string{message}, static_cast<int32_t>(c));
                }
            });

            win_emu.log.info("Using emulator backend: %s\n", win_emu.emu().get_name().c_str());

            {
                const std::lock_guard lock{this->mutex_};
                if (this->stop_requested_)
                {
                    return 1;
                }

                this->emulator_ = &win_emu;
                this->ui_ = ui_backend;
            }

            win_emu.start();

            {
                const std::lock_guard lock{this->mutex_};
                this->emulator_ = nullptr;
                this->ui_ = nullptr;
            }

            const auto exit_status = win_emu.process.exit_status;
            return exit_status ? static_cast<int>(*exit_status) : 1;
        }
        catch (const std::exception& e)
        {
            this->output(std::string{"Emulation failed: "} + e.what() + "\n", static_cast<int32_t>(color::red));
        }

        {
            const std::lock_guard lock{this->mutex_};
            this->emulator_ = nullptr;
            this->ui_ = nullptr;
        }

        return 1;
    }

    void session::stop()
    {
        const std::lock_guard lock{this->mutex_};

        this->stop_requested_ = true;
        if (this->emulator_)
        {
            this->emulator_->stop();
        }
    }

    void session::with_ui(const std::function<void(android_ui_backend&)>& action)
    {
        const std::lock_guard lock{this->mutex_};

        if (this->ui_)
        {
            action(*this->ui_);
        }
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
