#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

#ifdef SOGEN_HAS_SDL3
#include <SDL3/SDL.h>
#endif

namespace sogen
{
    namespace
    {
#ifdef SOGEN_HAS_SDL3
        constexpr uint32_t wm_keyup = 0x0101;
        constexpr uint32_t wm_char = 0x0102;

        uint64_t map_sdl_keycode(const SDL_Keycode key)
        {
            if (key >= 'a' && key <= 'z')
            {
                return static_cast<uint64_t>(key) - static_cast<uint64_t>('a') + static_cast<uint64_t>('A');
            }

            if ((key >= '0' && key <= '9') || key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_TAB ||
                key == SDLK_SPACE)
            {
                return static_cast<uint64_t>(key);
            }

            return 0;
        }
#endif

        class sdl_ui_backend final : public ui_backend
        {
          public:
#ifdef SOGEN_HAS_SDL3
            struct window_state
            {
                ui_window_desc desc{};
                SDL_Window* window{};
                SDL_Renderer* renderer{};
                SDL_Texture* texture{};
                int texture_width{};
                int texture_height{};
                ui_surface_format texture_format{ui_surface_format::bgra8};
                bool has_surface{};
            };
#endif
            ~sdl_ui_backend() override
            {
#ifdef SOGEN_HAS_SDL3
                for (auto& [guest, state] : this->windows_)
                {
                    (void)guest;
                    destroy_window_resources(state);
                }

                this->windows_.clear();
                this->guest_by_window_id_.clear();

                if (this->initialized_)
                {
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);
                }
#endif
            }

            void set_event_sink(event_sink sink) override
            {
                this->sink_ = std::move(sink);
            }

            void pump_events() override
            {
#ifdef SOGEN_HAS_SDL3
                if (!this->ensure_initialized())
                {
                    return;
                }

                SDL_Event event{};
                while (SDL_PollEvent(&event))
                {
                    switch (event.type)
                    {
                    case SDL_EVENT_QUIT:
                        for (const auto& [guest, window] : this->windows_)
                        {
                            (void)window;
                            this->post_event(guest, WM_CLOSE, 0, 0);
                        }
                        break;

                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                        if (const auto guest = this->resolve_guest(event.window.windowID); guest != 0)
                        {
                            this->post_event(guest, WM_CLOSE, 0, 0);
                        }
                        break;

                    case SDL_EVENT_KEY_DOWN:
                        if (!event.key.repeat)
                        {
                            if (const auto guest = this->resolve_guest(event.key.windowID); guest != 0)
                            {
                                if (const auto key = map_sdl_keycode(event.key.key); key != 0)
                                {
                                    this->post_event(guest, WM_KEYDOWN, key, 0);
                                }
                            }
                        }
                        break;

                    case SDL_EVENT_KEY_UP:
                        if (const auto guest = this->resolve_guest(event.key.windowID); guest != 0)
                        {
                            if (const auto key = map_sdl_keycode(event.key.key); key != 0)
                            {
                                this->post_event(guest, wm_keyup, key, 0);
                            }
                        }
                        break;

                    case SDL_EVENT_TEXT_INPUT:
                        if (const auto guest = this->resolve_guest(event.text.windowID); guest != 0)
                        {
                            const auto text = u8_to_u16(event.text.text ? std::string_view{event.text.text} : std::string_view{});
                            for (const auto ch : text)
                            {
                                this->post_event(guest, wm_char, ch, 0);
                            }
                        }
                        break;

                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        if (event.button.button == SDL_BUTTON_LEFT)
                        {
                            if (const auto guest = this->resolve_guest(event.button.windowID); guest != 0)
                            {
                                this->handle_left_click(guest, static_cast<int>(event.button.x), static_cast<int>(event.button.y));
                            }
                        }
                        break;

                    default:
                        break;
                    }
                }
#endif
            }

            void create_window(const ui_window_desc& desc) override
            {
#ifdef SOGEN_HAS_SDL3
                if (!this->ensure_initialized())
                {
                    return;
                }

                if (!desc.top_level)
                {
                    auto& state = this->windows_[desc.handle];
                    state.desc = desc;
                    this->redraw_related(desc.handle);
                    return;
                }

                Uint64 flags = 0;
                if (!desc.visible)
                {
                    flags |= SDL_WINDOW_HIDDEN;
                }
                if ((desc.style & WS_VISIBLE) != 0)
                {
                    flags &= ~SDL_WINDOW_HIDDEN;
                }
                if ((desc.style & WS_CHILD) == 0)
                {
                    flags |= SDL_WINDOW_RESIZABLE;
                }

                const auto width = std::max(1L, desc.rect.right - desc.rect.left);
                const auto height = std::max(1L, desc.rect.bottom - desc.rect.top);
                const auto title = u16_to_u8(desc.title);
                auto* window = SDL_CreateWindow(title.c_str(), static_cast<int>(width), static_cast<int>(height), flags);
                if (!window)
                {
                    return;
                }

                auto* renderer = SDL_CreateRenderer(window, nullptr);
                if (!renderer)
                {
                    SDL_DestroyWindow(window);
                    return;
                }

                SDL_SetWindowPosition(window, desc.rect.left, desc.rect.top);
                SDL_StartTextInput(window);

                auto& state = this->windows_[desc.handle];
                state.desc = desc;
                state.window = window;
                state.renderer = renderer;
                this->guest_by_window_id_[SDL_GetWindowID(window)] = desc.handle;
                this->render_window(state);
#endif
            }

            void destroy_window(const hwnd window) override
            {
#ifdef SOGEN_HAS_SDL3
                if (const auto it = this->windows_.find(window); it != this->windows_.end())
                {
                    SDL_StopTextInput(it->second.window);
                    this->guest_by_window_id_.erase(SDL_GetWindowID(it->second.window));
                    destroy_window_resources(it->second);
                    this->windows_.erase(it);
                }
#else
                (void)window;
#endif
            }

            void set_window_rect(const hwnd window, const RECT& rect) override
            {
#ifdef SOGEN_HAS_SDL3
                if (auto* state = this->resolve_window(window))
                {
                    state->desc.rect = rect;
                    if (state->desc.top_level)
                    {
                        SDL_SetWindowPosition(state->window, rect.left, rect.top);
                        SDL_SetWindowSize(state->window, rect.right - rect.left, rect.bottom - rect.top);
                    }
                    this->redraw_related(window);
                }
#else
                (void)window;
                (void)rect;
#endif
            }

            void set_window_visible(const hwnd window, const bool visible) override
            {
#ifdef SOGEN_HAS_SDL3
                if (auto* state = this->resolve_window(window))
                {
                    state->desc.visible = visible;
                    if (state->desc.top_level)
                    {
                        if (visible)
                        {
                            SDL_ShowWindow(state->window);
                        }
                        else
                        {
                            SDL_HideWindow(state->window);
                        }
                    }
                    this->redraw_related(window);
                }
#else
                (void)window;
                (void)visible;
#endif
            }

            void set_window_enabled(const hwnd window, const bool enabled) override
            {
#ifdef SOGEN_HAS_SDL3
                if (auto* state = this->resolve_window(window))
                {
                    state->desc.enabled = enabled;
                    this->redraw_related(window);
                }
#else
                (void)window;
                (void)enabled;
#endif
            }

            void set_window_title(const hwnd window, std::u16string_view title) override
            {
#ifdef SOGEN_HAS_SDL3
                if (auto* state = this->resolve_window(window))
                {
                    state->desc.title = std::u16string{title};
                    if (state->desc.top_level)
                    {
                        const auto utf8 = u16_to_u8(title);
                        SDL_SetWindowTitle(state->window, utf8.c_str());
                    }
                    this->redraw_related(window);
                }
#else
                (void)window;
                (void)title;
#endif
            }

            void present_surface(const hwnd window, const ui_surface_desc& surface) override
            {
#ifdef SOGEN_HAS_SDL3
                if (auto* state = this->resolve_window(window))
                {
                    present_surface(*state, surface);
                }
#else
                (void)window;
                (void)surface;
#endif
            }

          private:
#ifdef SOGEN_HAS_SDL3
            bool ensure_initialized()
            {
                if (!this->initialized_)
                {
                    this->initialized_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
                }

                return this->initialized_;
            }

            window_state* resolve_window(const hwnd window)
            {
                const auto it = this->windows_.find(window);
                return it == this->windows_.end() ? nullptr : &it->second;
            }

            const window_state* resolve_window(const hwnd window) const
            {
                const auto it = this->windows_.find(window);
                return it == this->windows_.end() ? nullptr : &it->second;
            }

            hwnd get_top_level_ancestor(hwnd window) const
            {
                while (true)
                {
                    const auto* state = this->resolve_window(window);
                    if (!state)
                    {
                        return 0;
                    }
                    if (state->desc.top_level || state->desc.parent == 0)
                    {
                        return window;
                    }
                    window = state->desc.parent;
                }
            }

            void redraw_related(const hwnd window)
            {
                if (const auto top = this->get_top_level_ancestor(window); top != 0)
                {
                    if (auto* top_state = this->resolve_window(top))
                    {
                        this->render_window(*top_state);
                    }
                }
            }

            static SDL_PixelFormat get_sdl_pixel_format(const ui_surface_format format)
            {
                switch (format)
                {
                case ui_surface_format::rgba8:
                    return SDL_PIXELFORMAT_RGBA32;
                case ui_surface_format::bgra8:
                default:
                    return SDL_PIXELFORMAT_BGRA32;
                }
            }

            static void destroy_window_resources(window_state& state)
            {
                if (state.texture)
                {
                    SDL_DestroyTexture(state.texture);
                    state.texture = nullptr;
                }
                if (state.renderer)
                {
                    SDL_DestroyRenderer(state.renderer);
                    state.renderer = nullptr;
                }
                if (state.window)
                {
                    SDL_DestroyWindow(state.window);
                    state.window = nullptr;
                }
                state.texture_width = 0;
                state.texture_height = 0;
            }

            static void ensure_texture(window_state& state, const ui_surface_desc& surface)
            {
                if (state.texture && state.texture_width == surface.width && state.texture_height == surface.height &&
                    state.texture_format == surface.format)
                {
                    return;
                }

                if (state.texture)
                {
                    SDL_DestroyTexture(state.texture);
                    state.texture = nullptr;
                }

                state.texture = SDL_CreateTexture(state.renderer, get_sdl_pixel_format(surface.format), SDL_TEXTUREACCESS_STREAMING,
                                                  surface.width, surface.height);
                if (state.texture)
                {
                    state.texture_width = surface.width;
                    state.texture_height = surface.height;
                    state.texture_format = surface.format;
                }
            }

            static void present_surface(window_state& state, const ui_surface_desc& surface)
            {
                state.has_surface = true;
                if (!surface.pixels || surface.width <= 0 || surface.height <= 0 || surface.stride <= 0)
                {
                    return;
                }

                ensure_texture(state, surface);
                if (!state.texture)
                {
                    return;
                }

                SDL_UpdateTexture(state.texture, nullptr, surface.pixels, surface.stride);
                SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
                SDL_RenderClear(state.renderer);
                SDL_RenderTexture(state.renderer, state.texture, nullptr, nullptr);
                SDL_RenderPresent(state.renderer);
            }

            static void draw_debug_text(SDL_Renderer* renderer, const int x, const int y, std::u16string_view text, const SDL_Color color)
            {
                const auto utf8 = u16_to_u8(text);
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
                SDL_RenderDebugText(renderer, static_cast<float>(x), static_cast<float>(y), utf8.c_str());
            }

            static SDL_FRect make_rect(const RECT& rect)
            {
                return SDL_FRect{static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
                                 static_cast<float>(rect.bottom - rect.top)};
            }

            void render_window(window_state& state)
            {
                if (state.has_surface)
                {
                    if (state.texture)
                    {
                        SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
                        SDL_RenderClear(state.renderer);
                        SDL_RenderTexture(state.renderer, state.texture, nullptr, nullptr);
                        SDL_RenderPresent(state.renderer);
                    }
                    return;
                }

                SDL_SetRenderDrawColor(state.renderer, 224, 224, 224, 255);
                SDL_RenderClear(state.renderer);

                for (const auto& [guest, child] : this->windows_)
                {
                    (void)guest;
                    if (child.desc.parent != state.desc.handle || !child.desc.visible)
                    {
                        continue;
                    }

                    const auto normalized = child.desc.class_name;
                    const auto rect = make_rect(child.desc.rect);

                    if (normalized == u"Static")
                    {
                        draw_debug_text(state.renderer, child.desc.rect.left, child.desc.rect.top + 6, child.desc.title,
                                              SDL_Color{0, 0, 0, 255});
                        continue;
                    }

                    if (normalized == u"Button")
                    {
                        const auto fill = child.desc.enabled ? SDL_Color{240, 240, 240, 255} : SDL_Color{192, 192, 192, 255};
                        SDL_SetRenderDrawColor(state.renderer, fill.r, fill.g, fill.b, fill.a);
                        SDL_RenderFillRect(state.renderer, &rect);
                        SDL_SetRenderDrawColor(state.renderer, 32, 32, 32, 255);
                        SDL_RenderRect(state.renderer, &rect);
                        draw_debug_text(state.renderer, child.desc.rect.left + 8, child.desc.rect.top + 8, child.desc.title,
                                              SDL_Color{0, 0, 0, 255});
                        continue;
                    }
                }

                SDL_RenderPresent(state.renderer);
            }

            void handle_left_click(const hwnd top_level, const int x, const int y)
            {
                for (const auto& [guest, child] : this->windows_)
                {
                    (void)guest;
                    if (child.desc.parent != top_level || !child.desc.visible || !child.desc.enabled || child.desc.class_name != u"Button")
                    {
                        continue;
                    }

                    if (x >= child.desc.rect.left && x < child.desc.rect.right && y >= child.desc.rect.top && y < child.desc.rect.bottom)
                    {
                        this->post_event(top_level, WM_COMMAND, child.desc.control_id, child.desc.handle);
                        return;
                    }
                }
            }

            static void present_test_pattern(window_state& state, const int width, const int height)
            {
                std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
                for (int y = 0; y < height; ++y)
                {
                    for (int x = 0; x < width; ++x)
                    {
                        const auto index = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                        const bool checker = ((x / 32) + (y / 32)) % 2 == 0;
                        pixels[index] = checker ? 0xFF204060u : 0xFF4060A0u;
                    }
                }

                present_surface(state, ui_surface_desc{.width = width,
                                                             .height = height,
                                                             .stride = width * static_cast<int>(sizeof(uint32_t)),
                                                             .format = ui_surface_format::bgra8,
                                                             .pixels = pixels.data()});
            }

            hwnd resolve_guest(const SDL_WindowID window_id) const
            {
                const auto it = this->guest_by_window_id_.find(window_id);
                return it == this->guest_by_window_id_.end() ? 0 : it->second;
            }
#endif

            void post_event(const hwnd window, const uint32_t message, const uint64_t w_param, const uint64_t l_param) const
            {
                if (this->sink_)
                {
                    this->sink_(ui_event{.window = window, .message = message, .wParam = w_param, .lParam = l_param});
                }
            }

            event_sink sink_{};
#ifdef SOGEN_HAS_SDL3
            bool initialized_{};
            std::unordered_map<hwnd, window_state> windows_{};
            std::unordered_map<SDL_WindowID, hwnd> guest_by_window_id_{};
#endif
        };
    }

    std::unique_ptr<ui_backend> create_sdl_ui_backend()
    {
        return std::make_unique<sdl_ui_backend>();
    }
}
