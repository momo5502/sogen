#include "native_presenter.hpp"
#include <backend_selection.hpp>

#include <iostream>

namespace sogen
{
    namespace
    {
        class pumping_backend final : public sogen::ui_backend
        {
          public:
            std::unique_ptr<sogen::ui_backend> backend = sogen::create_sdl_ui_backend();
            std::function<void()> refresh{};

            void pump_events() override
            {
                this->backend->pump_events();
                if (this->refresh)
                {
                    this->refresh();
                }
            }

            void set_event_sink(event_sink sink) override
            {
                this->backend->set_event_sink(std::move(sink));
            }

            void set_snapshot_request_sink(const std::function<void()>& sink) override
            {
                this->backend->set_snapshot_request_sink(sink);
            }

            void reset() override
            {
                (*this->backend).reset();
            }

            void create_window(const ui_window_desc& desc) override
            {
                this->backend->create_window(desc);
            }

            void destroy_window(const hwnd window) override
            {
                this->backend->destroy_window(window);
            }

            void set_window_relationship(const hwnd window, const hwnd parent, const hwnd owner, const bool top_level) override
            {
                this->backend->set_window_relationship(window, parent, owner, top_level);
            }

            void set_window_rect(const hwnd window, const RECT& rect) override
            {
                this->backend->set_window_rect(window, rect);
            }

            void set_window_visible(const hwnd window, const bool visible) override
            {
                this->backend->set_window_visible(window, visible);
            }

            void set_window_enabled(const hwnd window, const bool enabled) override
            {
                this->backend->set_window_enabled(window, enabled);
            }

            void set_window_title(const hwnd window, std::u16string_view title) override
            {
                this->backend->set_window_title(window, title);
            }

            void invalidate(const hwnd window, const std::optional<RECT>& rect) override
            {
                this->backend->invalidate(window, rect);
            }

            void present_surface(const hwnd window, const ui_surface_desc& surface) override
            {
                this->backend->present_surface(window, surface);
            }

            std::vector<std::string> vulkan_instance_extensions() override
            {
                return this->backend->vulkan_instance_extensions();
            }

            uint64_t create_vulkan_surface(const hwnd window, const uint64_t instance) override
            {
                return this->backend->create_vulkan_surface(window, instance);
            }

            std::optional<ui_extent> vulkan_surface_extent(const hwnd window) override
            {
                return this->backend->vulkan_surface_extent(window);
            }

            void* vulkan_get_instance_proc_addr() override
            {
                return this->backend->vulkan_get_instance_proc_addr();
            }

            void set_cursor_position(const hwnd window, const int32_t screen_x, const int32_t screen_y) override
            {
                this->backend->set_cursor_position(window, screen_x, screen_y);
            }

            void set_cursor_visibility(const bool visible) override
            {
                this->backend->set_cursor_visibility(visible);
            }

            void set_cursor_clip(const hwnd window, const std::optional<ui_rect>& screen_rect, const bool relative) override
            {
                this->backend->set_cursor_clip(window, screen_rect, relative);
            }
        };
    }
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: qt-raster-native <root> <kvm|unicorn> <guest-calculator>\n";
        return 1;
    }
    try
    {
        const std::string_view name{argv[2]};
        if (name != "kvm" && name != "unicorn")
        {
            throw std::invalid_argument("Select kvm or unicorn explicitly");
        }
        const auto backend = name == "kvm" ? sogen::backend_type::kvm : sogen::backend_type::unicorn;
        auto ui = std::make_unique<sogen::pumping_backend>();
        auto* pumping = ui.get();
        auto& display = *pumping->backend;
        sogen::emulator_interfaces interfaces{};
        interfaces.ui = std::move(ui);
        interfaces.audio = std::make_unique<sogen::null_audio_backend>();
        interfaces.console = std::make_unique<sogen::null_console_backend>();
        sogen::application_settings application{.application = argv[3]};
        application.arguments = {u"-platform", u"bridge"};
        sogen::emulator_callbacks callbacks{};
        callbacks.on_stdout = [](const std::string_view data) { std::cout << data << std::flush; };
        sogen::windows_emulator emulator{
            sogen::create_x86_64_emulator(backend), std::move(application),
            sogen::emulator_settings{.disable_logging = true, .emulation_root = argv[1], .root_mode = sogen::filesystem_mode::read_write},
            std::move(callbacks), std::move(interfaces)};
        sogen::qt_raster::native_presenter presenter{emulator, display};
        pumping->refresh = [&] { emulator.synchronize([&] { presenter.update(); }); };
        try
        {
            emulator.start();
        }
        catch (...)
        {
            pumping->refresh = {};
            presenter.clear();
            display.reset();
            throw;
        }
        pumping->refresh = {};
        presenter.clear();
        display.reset();
        if (emulator.process.exit_status != uint32_t{0})
        {
            throw std::runtime_error("Guest did not exit successfully");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
