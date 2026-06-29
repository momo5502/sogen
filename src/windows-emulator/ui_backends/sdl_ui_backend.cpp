#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

#ifdef SOGEN_HAS_SDL3
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace sogen
{
    namespace
    {
#ifdef SOGEN_HAS_SDL3
#ifdef _WIN32
        void apply_application_icon(SDL_Window* window)
        {
            auto* hwnd =
                static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
            if (!hwnd)
            {
                return;
            }
            HICON icon = LoadIconW(GetModuleHandleW(nullptr), L"GLFW_ICON");
            if (!icon)
            {
                return;
            }
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        }
#endif

        uint64_t pack_point(const int x, const int y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xFFFFu) << 16) |
                   static_cast<uint64_t>(static_cast<uint32_t>(x) & 0xFFFFu);
        }

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

            switch (key)
            {
            case SDLK_LEFT:
                return VK_LEFT;
            case SDLK_UP:
                return VK_UP;
            case SDLK_RIGHT:
                return VK_RIGHT;
            case SDLK_DOWN:
                return VK_DOWN;
            case SDLK_DELETE:
                return VK_DELETE;
            case SDLK_HOME:
                return VK_HOME;
            case SDLK_END:
                return VK_END;
            case SDLK_PAGEUP:
                return VK_PRIOR;
            case SDLK_PAGEDOWN:
                return VK_NEXT;
            case SDLK_INSERT:
                return VK_INSERT;
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
                return VK_SHIFT;
            case SDLK_LCTRL:
            case SDLK_RCTRL:
                return VK_CONTROL;
            case SDLK_LALT:
            case SDLK_RALT:
                return VK_MENU;
            case SDLK_LGUI:
                return VK_LWIN;
            case SDLK_RGUI:
                return VK_RWIN;
            case SDLK_CAPSLOCK:
                return VK_CAPITAL;
            case SDLK_GRAVE:
                return VK_OEM_3;
            case SDLK_MINUS:
                return VK_OEM_MINUS;
            case SDLK_EQUALS:
                return VK_OEM_PLUS;
            case SDLK_LEFTBRACKET:
                return VK_OEM_4;
            case SDLK_RIGHTBRACKET:
                return VK_OEM_6;
            case SDLK_BACKSLASH:
                return VK_OEM_5;
            case SDLK_SEMICOLON:
                return VK_OEM_1;
            case SDLK_APOSTROPHE:
                return VK_OEM_7;
            case SDLK_COMMA:
                return VK_OEM_COMMA;
            case SDLK_PERIOD:
                return VK_OEM_PERIOD;
            case SDLK_SLASH:
                return VK_OEM_2;
            case SDLK_KP_0:
                return VK_NUMPAD0;
            case SDLK_KP_1:
                return VK_NUMPAD1;
            case SDLK_KP_2:
                return VK_NUMPAD2;
            case SDLK_KP_3:
                return VK_NUMPAD3;
            case SDLK_KP_4:
                return VK_NUMPAD4;
            case SDLK_KP_5:
                return VK_NUMPAD5;
            case SDLK_KP_6:
                return VK_NUMPAD6;
            case SDLK_KP_7:
                return VK_NUMPAD7;
            case SDLK_KP_8:
                return VK_NUMPAD8;
            case SDLK_KP_9:
                return VK_NUMPAD9;
            case SDLK_KP_PERIOD:
                return VK_DECIMAL;
            case SDLK_KP_PLUS:
                return VK_ADD;
            case SDLK_KP_MINUS:
                return VK_SUBTRACT;
            case SDLK_KP_MULTIPLY:
                return VK_MULTIPLY;
            case SDLK_KP_DIVIDE:
                return VK_DIVIDE;
            case SDLK_KP_ENTER:
                return VK_RETURN;
            default:
                if (key >= SDLK_F1 && key <= SDLK_F12)
                {
                    return VK_F1 + static_cast<uint64_t>(key - SDLK_F1);
                }
                return 0;
            }
        }

        struct scancode_context
        {
            bool key_up = false;
            bool was_down = false;
            bool alt_context = false;
            uint16_t repeat_count = 1;
        };

        uint64_t map_sdl_scancode(const SDL_Scancode scancode, const scancode_context context = {})
        {
            struct scan_result
            {
                uint8_t scan_code = 0;
                bool extended = false;
            };

            const auto scan = [&]() -> scan_result {
                switch (scancode)
                {
                // Letters
                case SDL_SCANCODE_A:
                    return {.scan_code = 0x1E};
                case SDL_SCANCODE_B:
                    return {.scan_code = 0x30};
                case SDL_SCANCODE_C:
                    return {.scan_code = 0x2E};
                case SDL_SCANCODE_D:
                    return {.scan_code = 0x20};
                case SDL_SCANCODE_E:
                    return {.scan_code = 0x12};
                case SDL_SCANCODE_F:
                    return {.scan_code = 0x21};
                case SDL_SCANCODE_G:
                    return {.scan_code = 0x22};
                case SDL_SCANCODE_H:
                    return {.scan_code = 0x23};
                case SDL_SCANCODE_I:
                    return {.scan_code = 0x17};
                case SDL_SCANCODE_J:
                    return {.scan_code = 0x24};
                case SDL_SCANCODE_K:
                    return {.scan_code = 0x25};
                case SDL_SCANCODE_L:
                    return {.scan_code = 0x26};
                case SDL_SCANCODE_M:
                    return {.scan_code = 0x32};
                case SDL_SCANCODE_N:
                    return {.scan_code = 0x31};
                case SDL_SCANCODE_O:
                    return {.scan_code = 0x18};
                case SDL_SCANCODE_P:
                    return {.scan_code = 0x19};
                case SDL_SCANCODE_Q:
                    return {.scan_code = 0x10};
                case SDL_SCANCODE_R:
                    return {.scan_code = 0x13};
                case SDL_SCANCODE_S:
                    return {.scan_code = 0x1F};
                case SDL_SCANCODE_T:
                    return {.scan_code = 0x14};
                case SDL_SCANCODE_U:
                    return {.scan_code = 0x16};
                case SDL_SCANCODE_V:
                    return {.scan_code = 0x2F};
                case SDL_SCANCODE_W:
                    return {.scan_code = 0x11};
                case SDL_SCANCODE_X:
                    return {.scan_code = 0x2D};
                case SDL_SCANCODE_Y:
                    return {.scan_code = 0x15};
                case SDL_SCANCODE_Z:
                    return {.scan_code = 0x2C};

                // Number row
                case SDL_SCANCODE_1:
                    return {.scan_code = 0x02};
                case SDL_SCANCODE_2:
                    return {.scan_code = 0x03};
                case SDL_SCANCODE_3:
                    return {.scan_code = 0x04};
                case SDL_SCANCODE_4:
                    return {.scan_code = 0x05};
                case SDL_SCANCODE_5:
                    return {.scan_code = 0x06};
                case SDL_SCANCODE_6:
                    return {.scan_code = 0x07};
                case SDL_SCANCODE_7:
                    return {.scan_code = 0x08};
                case SDL_SCANCODE_8:
                    return {.scan_code = 0x09};
                case SDL_SCANCODE_9:
                    return {.scan_code = 0x0A};
                case SDL_SCANCODE_0:
                    return {.scan_code = 0x0B};

                // Basic keys
                case SDL_SCANCODE_ESCAPE:
                    return {.scan_code = 0x01};
                case SDL_SCANCODE_BACKSPACE:
                    return {.scan_code = 0x0E};
                case SDL_SCANCODE_TAB:
                    return {.scan_code = 0x0F};
                case SDL_SCANCODE_RETURN:
                    return {.scan_code = 0x1C};
                case SDL_SCANCODE_SPACE:
                    return {.scan_code = 0x39};

                // Modifiers
                case SDL_SCANCODE_LSHIFT:
                    return {.scan_code = 0x2A};
                case SDL_SCANCODE_RSHIFT:
                    return {.scan_code = 0x36};
                case SDL_SCANCODE_LCTRL:
                    return {.scan_code = 0x1D};
                case SDL_SCANCODE_RCTRL:
                    return {.scan_code = 0x1D, .extended = true};
                case SDL_SCANCODE_LALT:
                    return {.scan_code = 0x38};
                case SDL_SCANCODE_RALT:
                    return {.scan_code = 0x38, .extended = true};
                case SDL_SCANCODE_LGUI:
                    return {.scan_code = 0x5B, .extended = true};
                case SDL_SCANCODE_RGUI:
                    return {.scan_code = 0x5C, .extended = true};
                case SDL_SCANCODE_APPLICATION:
                    return {.scan_code = 0x5D, .extended = true};

                // Navigation cluster: extended
                case SDL_SCANCODE_INSERT:
                    return {.scan_code = 0x52, .extended = true};
                case SDL_SCANCODE_DELETE:
                    return {.scan_code = 0x53, .extended = true};
                case SDL_SCANCODE_HOME:
                    return {.scan_code = 0x47, .extended = true};
                case SDL_SCANCODE_END:
                    return {.scan_code = 0x4F, .extended = true};
                case SDL_SCANCODE_PAGEUP:
                    return {.scan_code = 0x49, .extended = true};
                case SDL_SCANCODE_PAGEDOWN:
                    return {.scan_code = 0x51, .extended = true};

                // Arrow keys: extended
                case SDL_SCANCODE_LEFT:
                    return {.scan_code = 0x4B, .extended = true};
                case SDL_SCANCODE_UP:
                    return {.scan_code = 0x48, .extended = true};
                case SDL_SCANCODE_RIGHT:
                    return {.scan_code = 0x4D, .extended = true};
                case SDL_SCANCODE_DOWN:
                    return {.scan_code = 0x50, .extended = true};

                // Locks
                case SDL_SCANCODE_CAPSLOCK:
                    return {.scan_code = 0x3A};
                case SDL_SCANCODE_NUMLOCKCLEAR:
                    return {.scan_code = 0x45};
                case SDL_SCANCODE_SCROLLLOCK:
                    return {.scan_code = 0x46};

                // Punctuation, US keyboard physical positions
                case SDL_SCANCODE_GRAVE:
                    return {.scan_code = 0x29};
                case SDL_SCANCODE_MINUS:
                    return {.scan_code = 0x0C};
                case SDL_SCANCODE_EQUALS:
                    return {.scan_code = 0x0D};
                case SDL_SCANCODE_LEFTBRACKET:
                    return {.scan_code = 0x1A};
                case SDL_SCANCODE_RIGHTBRACKET:
                    return {.scan_code = 0x1B};
                case SDL_SCANCODE_BACKSLASH:
                    return {.scan_code = 0x2B};
                case SDL_SCANCODE_NONUSHASH:
                    return {.scan_code = 0x2B};
                case SDL_SCANCODE_SEMICOLON:
                    return {.scan_code = 0x27};
                case SDL_SCANCODE_APOSTROPHE:
                    return {.scan_code = 0x28};
                case SDL_SCANCODE_COMMA:
                    return {.scan_code = 0x33};
                case SDL_SCANCODE_PERIOD:
                    return {.scan_code = 0x34};
                case SDL_SCANCODE_SLASH:
                    return {.scan_code = 0x35};
                case SDL_SCANCODE_NONUSBACKSLASH:
                    return {.scan_code = 0x56};

                // Function keys
                case SDL_SCANCODE_F1:
                    return {.scan_code = 0x3B};
                case SDL_SCANCODE_F2:
                    return {.scan_code = 0x3C};
                case SDL_SCANCODE_F3:
                    return {.scan_code = 0x3D};
                case SDL_SCANCODE_F4:
                    return {.scan_code = 0x3E};
                case SDL_SCANCODE_F5:
                    return {.scan_code = 0x3F};
                case SDL_SCANCODE_F6:
                    return {.scan_code = 0x40};
                case SDL_SCANCODE_F7:
                    return {.scan_code = 0x41};
                case SDL_SCANCODE_F8:
                    return {.scan_code = 0x42};
                case SDL_SCANCODE_F9:
                    return {.scan_code = 0x43};
                case SDL_SCANCODE_F10:
                    return {.scan_code = 0x44};
                case SDL_SCANCODE_F11:
                    return {.scan_code = 0x57};
                case SDL_SCANCODE_F12:
                    return {.scan_code = 0x58};

                // Extended F keys, usually not needed for older Win32 games,
                // but harmless if something asks for them.
                case SDL_SCANCODE_F13:
                    return {.scan_code = 0x64};
                case SDL_SCANCODE_F14:
                    return {.scan_code = 0x65};
                case SDL_SCANCODE_F15:
                    return {.scan_code = 0x66};

                // Keypad
                case SDL_SCANCODE_KP_7:
                    return {.scan_code = 0x47};
                case SDL_SCANCODE_KP_8:
                    return {.scan_code = 0x48};
                case SDL_SCANCODE_KP_9:
                    return {.scan_code = 0x49};
                case SDL_SCANCODE_KP_MINUS:
                    return {.scan_code = 0x4A};
                case SDL_SCANCODE_KP_4:
                    return {.scan_code = 0x4B};
                case SDL_SCANCODE_KP_5:
                    return {.scan_code = 0x4C};
                case SDL_SCANCODE_KP_6:
                    return {.scan_code = 0x4D};
                case SDL_SCANCODE_KP_PLUS:
                    return {.scan_code = 0x4E};
                case SDL_SCANCODE_KP_1:
                    return {.scan_code = 0x4F};
                case SDL_SCANCODE_KP_2:
                    return {.scan_code = 0x50};
                case SDL_SCANCODE_KP_3:
                    return {.scan_code = 0x51};
                case SDL_SCANCODE_KP_0:
                    return {.scan_code = 0x52};
                case SDL_SCANCODE_KP_PERIOD:
                    return {.scan_code = 0x53};
                case SDL_SCANCODE_KP_MULTIPLY:
                    return {.scan_code = 0x37};
                case SDL_SCANCODE_KP_DIVIDE:
                    return {.scan_code = 0x35, .extended = true};
                case SDL_SCANCODE_KP_ENTER:
                    return {.scan_code = 0x1C, .extended = true};

                // Special keys
                case SDL_SCANCODE_PRINTSCREEN:
                    return {.scan_code = 0x37, .extended = true};
                case SDL_SCANCODE_PAUSE:
                    return {.scan_code = 0x45};
                case SDL_SCANCODE_MENU:
                    return {.scan_code = 0x5D, .extended = true};

                default:
                    return {};
                }
            }();

            if (scan.scan_code == 0)
            {
                return 0;
            }

            uint64_t scan_code = 0;

            // bits 0-15: repeat count
            scan_code |= static_cast<uint64_t>(context.repeat_count == 0 ? 1 : context.repeat_count);

            // bits 16-23: scan code
            scan_code |= static_cast<uint64_t>(scan.scan_code) << 16;

            // bit 24: extended key
            if (scan.extended)
            {
                scan_code |= 1ull << 24;
            }

            // bit 29: context code, normally ALT/syskey context
            if (context.alt_context)
            {
                scan_code |= 1ull << 29;
            }

            // bit 30: previous key state
            if (context.was_down)
            {
                scan_code |= 1ull << 30;
            }

            // bit 31: transition state, set for key release
            if (context.key_up)
            {
                scan_code |= 1ull << 31;
            }

            return scan_code;
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
                this->reset();

                if (this->initialized_)
                {
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);
                }
#endif
            }

            void reset() override
            {
#ifdef SOGEN_HAS_SDL3
                for (auto& [guest, state] : this->windows_)
                {
                    (void)guest;
                    destroy_window_resources(state);
                }

                this->windows_.clear();
                this->guest_by_window_id_.clear();
                this->active_window_ = 0;
                this->key_down_.fill(false);

                if (this->initialized_)
                {
                    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
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

                    // Window focus/cursor transitions drive activation. Games gate their per-frame mouse
                    // polling on WM_ACTIVATE (in_appactive) plus the foreground window, so a window that is
                    // never told it became active never reads the mouse.
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    case SDL_EVENT_WINDOW_MOUSE_ENTER:
                        this->set_window_active(this->resolve_guest(event.window.windowID), true);
                        break;

                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                        this->set_window_active(this->resolve_guest(event.window.windowID), false);
                        break;

                    // The guest renders at its own fixed resolution; the host window can be any size. On a
                    // host resize/expose, re-present the last frame so the letterbox (aspect-preserving fit
                    // with black bars) updates immediately, even if the guest has not produced a new frame.
                    case SDL_EVENT_WINDOW_RESIZED:
                    case SDL_EVENT_WINDOW_MAXIMIZED:
                    case SDL_EVENT_WINDOW_RESTORED:
                    case SDL_EVENT_WINDOW_EXPOSED:
                        if (auto* state = this->resolve_window(this->resolve_guest(event.window.windowID)))
                        {
                            render_window(*state);
                        }
                        break;

                    case SDL_EVENT_KEY_DOWN: {
                        const auto guest = this->resolve_guest(event.key.windowID);
                        if (guest == 0)
                        {
                            break;
                        }

                        const auto vk = map_sdl_keycode(event.key.key);
                        if (vk == 0)
                        {
                            break;
                        }

                        bool was_down = event.key.repeat;
                        const auto scancode_index = static_cast<size_t>(event.key.scancode);

                        if (scancode_index < key_down_.size())
                        {
                            was_down = was_down || key_down_[scancode_index];
                        }

                        const bool is_alt = vk == VK_MENU;
                        const bool alt_down = (event.key.mod & SDL_KMOD_ALT) != 0;
                        const bool alt_context = !is_alt && alt_down;

                        // TODO: Track repeat count
                        scancode_context context{.was_down = was_down, .alt_context = alt_context};
                        const auto scan = map_sdl_scancode(event.key.scancode, context);
                        if (scan == 0)
                        {
                            break;
                        }

                        if (scancode_index < key_down_.size())
                        {
                            key_down_[scancode_index] = true;
                        }

                        const uint32_t message = (is_alt || vk == VK_F10 || alt_context) ? WM_SYSKEYDOWN : WM_KEYDOWN;

                        this->post_event(guest, message, vk, scan);
                        break;
                    }

                    case SDL_EVENT_KEY_UP: {
                        const auto guest = this->resolve_guest(event.key.windowID);
                        if (guest == 0)
                        {
                            break;
                        }

                        const auto vk = map_sdl_keycode(event.key.key);
                        if (vk == 0)
                        {
                            break;
                        }

                        const bool is_alt = vk == VK_MENU;
                        const bool alt_down = (event.key.mod & SDL_KMOD_ALT) != 0;
                        const bool alt_context = !is_alt && alt_down;

                        scancode_context context{.key_up = true, .was_down = true, .alt_context = alt_context};
                        const auto scan = map_sdl_scancode(event.key.scancode, context);
                        if (scan == 0)
                        {
                            break;
                        }

                        const auto scancode_index = static_cast<size_t>(event.key.scancode);
                        if (scancode_index < key_down_.size())
                        {
                            key_down_[scancode_index] = false;
                        }

                        const uint32_t message = (is_alt || vk == VK_F10 || alt_context) ? WM_SYSKEYUP : WM_KEYUP;

                        this->post_event(guest, message, vk, scan);
                        break;
                    }

                    case SDL_EVENT_TEXT_INPUT:
                        if (const auto guest = this->resolve_guest(event.text.windowID); guest != 0)
                        {
                            const auto text = u8_to_u16(event.text.text ? std::string_view{event.text.text} : std::string_view{});
                            for (const auto ch : text)
                            {
                                this->post_event(guest, WM_CHAR, ch, 0);
                            }
                        }
                        break;

                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        if (event.button.button == SDL_BUTTON_LEFT)
                        {
                            if (const auto guest = this->resolve_guest(event.button.windowID); guest != 0)
                            {
                                this->set_window_active(guest, true);
                                this->post_event(guest, WM_LBUTTONDOWN, MK_LBUTTON,
                                                 this->map_window_point(guest, event.button.x, event.button.y));
                            }
                        }
                        break;

                    case SDL_EVENT_MOUSE_BUTTON_UP:
                        if (event.button.button == SDL_BUTTON_LEFT)
                        {
                            if (const auto guest = this->resolve_guest(event.button.windowID); guest != 0)
                            {
                                this->post_event(guest, WM_LBUTTONUP, 0, this->map_window_point(guest, event.button.x, event.button.y));
                            }
                        }
                        break;

                    case SDL_EVENT_MOUSE_MOTION:
                        if (const auto guest = this->resolve_guest(event.motion.windowID); guest != 0)
                        {
                            this->set_window_active(guest, true);
                            const uint64_t keys = (event.motion.state & SDL_BUTTON_LMASK) ? MK_LBUTTON : 0;
                            this->post_event(guest, WM_MOUSEMOVE, keys, this->map_window_point(guest, event.motion.x, event.motion.y));
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
                // A window without a full caption (e.g. a WS_POPUP splash screen) has no title bar and no
                // minimize/maximize/close controls; mirror that on the host window.
                if ((desc.style & WS_CAPTION) != WS_CAPTION)
                {
                    flags |= SDL_WINDOW_BORDERLESS;
                }
                // Only windows with a sizing border (WS_THICKFRAME) can be resized by the user.
                if ((desc.style & WS_CHILD) == 0 && (desc.style & WS_THICKFRAME) != 0)
                {
                    flags |= SDL_WINDOW_RESIZABLE;
                }

                const auto width = std::max<int>(1, static_cast<int>(desc.rect.right - desc.rect.left));
                const auto height = std::max<int>(1, static_cast<int>(desc.rect.bottom - desc.rect.top));
                const auto title = u16_to_u8(desc.title);
                auto* window = SDL_CreateWindow(title.c_str(), static_cast<int>(width), static_cast<int>(height), flags);
                if (!window)
                {
                    return;
                }

#ifdef _WIN32
                apply_application_icon(window);
#endif

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
                render_window(state);
#endif
            }

            void destroy_window(const hwnd window) override
            {
#ifdef SOGEN_HAS_SDL3
                if (const auto it = this->windows_.find(window); it != this->windows_.end())
                {
                    // Child (non-top-level) windows have no SDL window; only top-level windows do.
                    if (it->second.window)
                    {
                        SDL_StopTextInput(it->second.window);
                        this->guest_by_window_id_.erase(SDL_GetWindowID(it->second.window));
                    }
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
                // The render target is frequently a child window (e.g. a D3D/Vulkan swap-chain child),
                // which has no SDL renderer of its own. Composite its surface onto the top-level
                // ancestor -- the window that actually owns the on-screen SDL window and renderer.
                auto* state = this->resolve_window(window);
                if (state && !state->renderer)
                {
                    state = this->resolve_window(this->get_top_level_ancestor(window));
                }
                if (state && state->renderer)
                {
                    update_surface_texture(*state, surface);
                    render_window(*state);
                }
#else
                (void)window;
                (void)surface;
#endif
            }

            void set_cursor_position(const hwnd window, const int32_t screen_x, const int32_t screen_y) override
            {
#ifdef SOGEN_HAS_SDL3
                // Top-levels are positioned at their emulated rect (SDL_SetWindowPosition), so emulated screen
                // coordinates map to window-local by subtracting that origin.
                const auto top = this->get_top_level_ancestor(window);
                if (auto* state = this->resolve_window(top); state && state->window)
                {
                    SDL_WarpMouseInWindow(state->window, static_cast<float>(screen_x - state->desc.rect.left),
                                          static_cast<float>(screen_y - state->desc.rect.top));
                }
#else
                (void)window;
                (void)screen_x;
                (void)screen_y;
#endif
            }

          private:
#ifdef SOGEN_HAS_SDL3
            bool ensure_initialized()
            {
                if (!this->initialized_)
                {
                    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
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
                // Bound the walk so a malformed parent chain (stale handles, a cycle) can't loop forever.
                constexpr auto max_depth = 32;
                for (auto depth = 0; depth < max_depth; ++depth)
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

                return window;
            }

            void redraw_related(const hwnd window)
            {
                if (const auto top = this->get_top_level_ancestor(window); top != 0)
                {
                    if (auto* top_state = this->resolve_window(top))
                    {
                        render_window(*top_state);
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
                    SDL_SetTextureBlendMode(state.texture, SDL_BLENDMODE_NONE);
                    state.texture_width = surface.width;
                    state.texture_height = surface.height;
                    state.texture_format = surface.format;
                }
            }

            static void update_surface_texture(window_state& state, const ui_surface_desc& surface)
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
            }

            static void render_window(window_state& state)
            {
                if (state.has_surface && state.texture)
                {
                    // The guest renders at a fixed resolution; fit it into the (independently sized) host window
                    // preserving aspect ratio, with black bars on the mismatched axis. Letterbox presentation also
                    // makes SDL_RenderCoordinatesFromWindow map host mouse positions back to guest pixels.
                    SDL_SetRenderLogicalPresentation(state.renderer, state.texture_width, state.texture_height,
                                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
                    SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
                    SDL_RenderClear(state.renderer);
                    SDL_RenderTexture(state.renderer, state.texture, nullptr, nullptr);
                    SDL_RenderPresent(state.renderer);
                    return;
                }

                SDL_SetRenderLogicalPresentation(state.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
                SDL_SetRenderDrawColor(state.renderer, 224, 224, 224, 255);
                SDL_RenderClear(state.renderer);
                SDL_RenderPresent(state.renderer);
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

                update_surface_texture(state, ui_surface_desc{.width = width,
                                                              .height = height,
                                                              .stride = width * static_cast<int>(sizeof(uint32_t)),
                                                              .format = ui_surface_format::bgra8,
                                                              .pixels = pixels.data()});
                SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
                SDL_RenderClear(state.renderer);
                if (state.texture)
                {
                    SDL_RenderTexture(state.renderer, state.texture, nullptr, nullptr);
                }
                SDL_RenderPresent(state.renderer);
            }

            hwnd resolve_guest(const SDL_WindowID window_id) const
            {
                const auto it = this->guest_by_window_id_.find(window_id);
                return it == this->guest_by_window_id_.end() ? 0 : it->second;
            }

            // Map a host-window mouse position to guest client coordinates. With letterbox logical presentation,
            // SDL_RenderCoordinatesFromWindow undoes the fit-and-center transform so clicks land on the right pixel.
            uint64_t map_window_point(const hwnd guest, const float window_x, const float window_y) const
            {
                float render_x = window_x;
                float render_y = window_y;
                if (const auto* state = this->resolve_window(guest); state && state->renderer)
                {
                    SDL_RenderCoordinatesFromWindow(state->renderer, window_x, window_y, &render_x, &render_y);
                }
                return pack_point(static_cast<int>(render_x), static_cast<int>(render_y));
            }

            void set_window_active(const hwnd window, const bool active)
            {
                if (window == 0)
                {
                    return;
                }

                if (active)
                {
                    if (this->active_window_ == window)
                    {
                        return;
                    }
                    this->active_window_ = window;
                    this->post_event(window, WM_SETFOCUS, 0, 0);
                    this->post_event(window, WM_ACTIVATE, WA_ACTIVE, 0);
                }
                else
                {
                    if (this->active_window_ != window)
                    {
                        return;
                    }
                    this->active_window_ = 0;
                    this->post_event(window, WM_ACTIVATE, WA_INACTIVE, 0);
                    this->post_event(window, WM_KILLFOCUS, 0, 0);
                }
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
            hwnd active_window_{};
            std::array<bool, SDL_SCANCODE_COUNT> key_down_{};
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
