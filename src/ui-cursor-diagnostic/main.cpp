#include <platform/platform.hpp>
#include <platform/ui_backend.hpp>
#include <utils/finally.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    constexpr std::array<uint32_t, 4> cursors{32512, 32513, 32644, 32649};
    constexpr sogen::hwnd window_id = 1;

    void require(bool value, const char* message)
    {
        if (!value)
        {
            throw std::runtime_error(message);
        }
    }

    int check_cursors(std::unique_ptr<sogen::ui_backend>& ui)
    {
        auto* fallback = SDL_GetDefaultCursor();
        auto worker = std::thread([&] { ui->set_system_cursor(cursors[1]); });
        worker.join();
        require(SDL_GetCursor() == fallback, "A cursor request ran before the UI pump");
        SDL_ClearError();
        ui->pump_events();
        auto* text = SDL_GetCursor();
        if (text == fallback && *SDL_GetError() != '\0')
        {
            std::cerr << "System cursors unavailable: " << SDL_GetError() << '\n';
            return 77;
        }
        require(text != nullptr && text != fallback, "Text cursor was not selected");

        worker = std::thread([&] { ui->set_system_cursor(cursors[2]); });
        worker.join();
        require(SDL_GetCursor() == text, "A worker changed the selected cursor");
        ui->pump_events();
        auto* resize = SDL_GetCursor();
        require(resize != nullptr && resize != text && resize != fallback, "Resize cursor was not selected");
        ui->set_system_cursor(cursors[1]);
        ui->pump_events();
        require(SDL_GetCursor() == text, "Repeated cursor selection did not reuse its resource");

        ui->set_system_cursor(cursors[0]);
        ui->pump_events();
        auto* arrow = SDL_GetCursor();
        ui->set_system_cursor(0xFFFFFFFF);
        ui->pump_events();
        require(SDL_GetCursor() == arrow, "An unknown cursor did not fall back to the arrow");

        for (size_t i = 0; i < 10; ++i)
        {
            ui->set_system_cursor(cursors[1]);
            (*ui).reset();
            require(SDL_GetCursor() == fallback, "Reset did not restore the default cursor");
            ui->pump_events();
            require(SDL_GetCursor() == fallback, "A queued request survived reset");
            ui->set_system_cursor(cursors[1]);
            ui->pump_events();
            require(SDL_GetCursor() != fallback, "Cursor selection failed after reset");
        }
        ui = nullptr;
        require(SDL_GetCursor() == fallback, "Destruction did not restore the default cursor");
        std::cout << "System cursor checks passed\n";
        return 0;
    }

    void show_cursors(sogen::ui_backend& ui)
    {
        bool running = true;
        ui.set_event_sink([&](const sogen::ui_event& event) {
            if (event.message == WM_CLOSE || (event.message == WM_KEYDOWN && event.wParam == 0x1B))
            {
                running = false;
            }
            else if (event.message == WM_MOUSEMOVE)
            {
                const auto x = static_cast<int16_t>(event.lParam & 0xFFFF);
                const auto column = static_cast<size_t>(std::clamp<int>(x / 160, 0, 3));
                ui.set_system_cursor(cursors[column]);
            }
            else if (event.message == WM_KEYDOWN && event.wParam >= '1' && event.wParam <= '4')
            {
                ui.set_system_cursor(cursors[static_cast<size_t>(event.wParam - '1')]);
            }
        });
        ui.create_window({.handle = window_id,
                          .rect = {.left = 100, .top = 100, .right = 740, .bottom = 340},
                          .title = u"Arrow | Text | Resize | Hand — move across panels or press 1–4; Esc closes",
                          .style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE,
                          .visible = true,
                          .top_level = true});
        std::vector<uint32_t> pixels(640 * 240);
        constexpr std::array<uint32_t, 4> colors{0xFFDFEAF4, 0xFFD8EEDF, 0xFFF8E7C2, 0xFFEADFF2};
        for (size_t y = 0; y < 240; ++y)
        {
            for (size_t x = 0; x < 640; ++x)
            {
                pixels[y * 640 + x] = colors[x / 160];
            }
        }
        ui.present_surface(window_id, {.width = 640, .height = 240, .stride = 640 * 4, .pixels = pixels.data()});
        while (running)
        {
            ui.pump_events();
            SDL_Delay(8);
        }
    }
}

int main(int argc, char** argv)
{
    const bool check = argc == 2 && std::string_view{argv[1]} == "--check";
    if (argc != 1 && !check)
    {
        std::cerr << "Usage: ui-cursor-diagnostic [--check]\n";
        return 1;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        std::cerr << "Video unavailable: " << SDL_GetError() << '\n';
        return 77;
    }
    const auto cleanup = sogen::utils::finally([] { SDL_QuitSubSystem(SDL_INIT_VIDEO); });
    if (!SDL_GetDefaultCursor())
    {
        std::cerr << "The video driver does not provide system cursors\n";
        return 77;
    }
    try
    {
        auto ui = sogen::create_sdl_ui_backend();
        if (check)
        {
            return check_cursors(ui);
        }
        show_cursors(*ui);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
