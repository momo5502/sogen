#include "../std_include.hpp"
#include <platform/ui_backend.hpp>

#ifdef OS_EMSCRIPTEN
#include <emscripten.h>

namespace sogen
{
    namespace
    {
        struct queued_web_ui_event
        {
            ui_event event{};
        };

        std::mutex g_web_ui_event_mutex{};
        std::deque<queued_web_ui_event> g_web_ui_events{};

        class web_ui_backend;
        web_ui_backend* g_active_web_ui_backend = nullptr;

        // clang-format off
        EM_JS(void, initialize_web_ui_bridge, (), {
            if (globalThis.__sogenUiBridgeInitialized) {
                return;
            }

            globalThis.__sogenUiBridgeInitialized = true;

            if (typeof globalThis.addEventListener !== 'function' || typeof globalThis.postMessage !== 'function') {
                return;
            }

            globalThis.addEventListener('message', function(event) {
                const message = event.data;
                if (!message || message.type !== 'sogen_ui_event') {
                    return;
                }

                Module._sogen_web_ui_push_event(
                    message.window >>> 0,
                    message.message >>> 0,
                    message.wParam >>> 0,
                    message.lParam >>> 0
                );
            });

            postMessage({ type: 'sogen_ui', command: 'host_ready' });
        });
        // clang-format on

        void post_ui_message(const char* command, const hwnd window)
        {
            // clang-format off
            EM_ASM({
                postMessage({
                    type: 'sogen_ui',
                    command: UTF8ToString($0),
                    hwnd: Number($1 >>> 0),
                });
            }, command, static_cast<uint32_t>(window));
            // clang-format on
        }

        void post_ui_rect_message(const char* command, const hwnd window, const RECT& rect)
        {
            // clang-format off
            EM_ASM({
                postMessage({
                    type: 'sogen_ui',
                    command: UTF8ToString($0),
                    hwnd: Number($1 >>> 0),
                    rect: {
                        left: $2,
                        top: $3,
                        right: $4,
                        bottom: $5,
                    },
                });
            }, command, static_cast<uint32_t>(window), rect.left, rect.top, rect.right, rect.bottom);
            // clang-format on
        }

        void post_ui_visibility_message(const char* command, const hwnd window, const bool value)
        {
            // clang-format off
            EM_ASM({
                postMessage({
                    type: 'sogen_ui',
                    command: UTF8ToString($0),
                    hwnd: Number($1 >>> 0),
                    value: Boolean($2),
                });
            }, command, static_cast<uint32_t>(window), value ? 1 : 0);
            // clang-format on
        }

        void post_ui_title_message(const char* command, const hwnd window, const std::u16string_view title)
        {
            const auto utf8 = u16_to_u8(title);
            // clang-format off
            EM_ASM({
                postMessage({
                    type: 'sogen_ui',
                    command: UTF8ToString($0),
                    hwnd: Number($1 >>> 0),
                    title: UTF8ToString($2),
                });
            }, command, static_cast<uint32_t>(window), utf8.c_str());
            // clang-format on
        }

        void post_ui_create_window(const ui_window_desc& desc)
        {
            const auto class_name = u16_to_u8(desc.class_name);
            const auto title = u16_to_u8(desc.title);
            // clang-format off
            EM_ASM({
                postMessage({
                    type: 'sogen_ui',
                    command: 'create_window',
                    hwnd: Number($0 >>> 0),
                    parent: Number($1 >>> 0),
                    owner: Number($2 >>> 0),
                    rect: {
                        left: $3,
                        top: $4,
                        right: $5,
                        bottom: $6,
                    },
                    class_name: UTF8ToString($7),
                    title: UTF8ToString($8),
                    style: Number($9 >>> 0),
                    ex_style: Number($10 >>> 0),
                    control_id: Number($11 >>> 0),
                    visible: Boolean($12),
                    enabled: Boolean($13),
                    top_level: Boolean($14),
                });
            }, static_cast<uint32_t>(desc.handle), static_cast<uint32_t>(desc.parent), static_cast<uint32_t>(desc.owner), desc.rect.left,
               desc.rect.top, desc.rect.right, desc.rect.bottom, class_name.c_str(), title.c_str(), desc.style, desc.ex_style,
               desc.control_id, desc.visible ? 1 : 0, desc.enabled ? 1 : 0, desc.top_level ? 1 : 0);
            // clang-format on
        }

        void post_ui_present_surface(const hwnd window, const ui_surface_desc& surface)
        {
            if (!surface.pixels || surface.width <= 0 || surface.height <= 0 || surface.stride <= 0)
            {
                return;
            }

            const auto byte_count = static_cast<size_t>(surface.stride) * static_cast<size_t>(surface.height);
            std::vector<std::byte> pixels(byte_count);
            std::memcpy(pixels.data(), surface.pixels, byte_count);

            const auto* data = reinterpret_cast<const uint8_t*>(pixels.data());
            // clang-format off
            EM_ASM({
                const pixels = HEAPU8.slice($5, $5 + $6);
                postMessage({
                    type: 'sogen_ui',
                    command: 'present_surface',
                    hwnd: Number($0 >>> 0),
                    width: $1,
                    height: $2,
                    stride: $3,
                    format: Number($4),
                    pixels,
                }, [pixels.buffer]);
            }, static_cast<uint32_t>(window), surface.width, surface.height, surface.stride, static_cast<int>(surface.format), data,
               byte_count);
            // clang-format on
        }

        void enqueue_web_ui_event(const ui_event& event)
        {
            std::scoped_lock lock{g_web_ui_event_mutex};
            g_web_ui_events.push_back(queued_web_ui_event{.event = event});
        }

        class web_ui_backend final : public ui_backend
        {
          public:
            web_ui_backend()
            {
                initialize_web_ui_bridge();
                g_active_web_ui_backend = this;
            }

            ~web_ui_backend() override
            {
                if (g_active_web_ui_backend == this)
                {
                    g_active_web_ui_backend = nullptr;
                }
            }

            void set_event_sink(event_sink sink) override
            {
                this->sink_ = std::move(sink);
            }

            void pump_events() override
            {
                std::deque<queued_web_ui_event> events{};
                {
                    std::scoped_lock lock{g_web_ui_event_mutex};
                    events.swap(g_web_ui_events);
                }

                if (this->sink_)
                {
                    for (const auto& event : events)
                    {
                        this->sink_(event.event);
                    }
                }

                emscripten_sleep(0);
            }

            void deliver_external_event(const ui_event& event)
            {
                if (this->sink_)
                {
                    this->sink_(event);
                    return;
                }

                enqueue_web_ui_event(event);
            }

            void create_window(const ui_window_desc& desc) override
            {
                post_ui_create_window(desc);
            }

            void destroy_window(const hwnd window) override
            {
                post_ui_message("destroy_window", window);
            }

            void set_window_rect(const hwnd window, const RECT& rect) override
            {
                post_ui_rect_message("set_rect", window, rect);
            }

            void set_window_visible(const hwnd window, const bool visible) override
            {
                post_ui_visibility_message("set_visible", window, visible);
            }

            void set_window_enabled(const hwnd window, const bool enabled) override
            {
                post_ui_visibility_message("set_enabled", window, enabled);
            }

            void set_window_title(const hwnd window, const std::u16string_view title) override
            {
                post_ui_title_message("set_title", window, title);
            }

            void invalidate(const hwnd window, const std::optional<RECT>& rect) override
            {
                if (rect)
                {
                    post_ui_rect_message("invalidate", window, *rect);
                }
                else
                {
                    post_ui_message("invalidate", window);
                }
            }

            void present_surface(const hwnd window, const ui_surface_desc& surface) override
            {
                post_ui_present_surface(window, surface);
            }

          private:
            event_sink sink_{};
        };
    }

    std::unique_ptr<ui_backend> create_web_ui_backend()
    {
        return std::make_unique<web_ui_backend>();
    }

    extern "C" EMSCRIPTEN_KEEPALIVE void sogen_web_ui_push_event(const uint32_t window, const uint32_t message, const uint32_t wparam,
                                                                 const uint32_t lparam)
    {
        const ui_event event{.window = window, .message = message, .wParam = wparam, .lParam = lparam};
        if (g_active_web_ui_backend)
        {
            g_active_web_ui_backend->deliver_external_event(event);
            return;
        }

        enqueue_web_ui_event(event);
    }
}

#else

namespace sogen
{
    std::unique_ptr<ui_backend> create_web_ui_backend()
    {
        return std::make_unique<null_ui_backend>();
    }
}

#endif
