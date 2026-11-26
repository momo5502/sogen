#include <platform/platform.hpp>
#include <platform/ui_backend.hpp>
#include <utils/finally.hpp>

#include "../windows-emulator/vulkan_handle_utils.hpp"

#include <SDL3/SDL.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <SDL3/SDL_vulkan.h>

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

    void keep_sdl_rendering_off_vulkan()
    {
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0", SDL_HINT_OVERRIDE);
    }

    int check_vulkan_windows(sogen::ui_backend& ui)
    {
        keep_sdl_rendering_off_vulkan();

        if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            std::cerr << "Vulkan unavailable: " << SDL_GetError() << '\n';
            return 77;
        }
        SDL_Vulkan_UnloadLibrary();

        ui.pump_events();
        ui.create_window({.handle = window_id,
                          .rect = {.left = 0, .top = 0, .right = 64, .bottom = 48},
                          .style = WS_POPUP,
                          .visible = false,
                          .top_level = true});
        require(SDL_Vulkan_GetVkGetInstanceProcAddr() == nullptr, "A window without a Vulkan surface loaded the Vulkan loader");

        const auto extensions = ui.vulkan_instance_extensions();
        const auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        const auto create_instance =
            get_instance_proc_addr ? reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc_addr(nullptr, "vkCreateInstance")) : nullptr;

        std::vector<const char*> names{};
        names.reserve(extensions.size());
        for (const auto& extension : extensions)
        {
            names.push_back(extension.c_str());
        }
        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.enabledExtensionCount = static_cast<uint32_t>(names.size());
        create_info.ppEnabledExtensionNames = names.data();
        VkInstance instance{};
        if (extensions.empty() || !create_instance || create_instance(&create_info, nullptr, &instance) != VK_SUCCESS)
        {
            std::cerr << "No usable Vulkan driver for a surface\n";
            return 77;
        }
        const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_instance_proc_addr(instance, "vkDestroyInstance"));
        const auto destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(get_instance_proc_addr(instance, "vkDestroySurfaceKHR"));

        const auto surface = ui.create_vulkan_surface(window_id, sogen::pack_vulkan_handle(instance));
        if (surface != 0)
        {
            destroy_surface(instance, sogen::unpack_vulkan_handle<VkSurfaceKHR>(surface), nullptr);
        }
        ui.destroy_window(window_id);
        destroy_instance(instance, nullptr);
        require(surface != 0, "The window got no Vulkan surface");

        std::cout << "Vulkan window checks passed\n";
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
    const bool check_vulkan = argc == 2 && std::string_view{argv[1]} == "--check-vulkan-windows";
    if (argc != 1 && !check && !check_vulkan)
    {
        std::cerr << "Usage: ui-cursor-diagnostic [--check | --check-vulkan-windows]\n";
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
        if (check_vulkan)
        {
            return check_vulkan_windows(*ui);
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
