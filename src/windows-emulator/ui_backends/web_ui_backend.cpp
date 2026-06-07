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

        // The guest models windows without a non-client area (client rect == window rect),
        // so the presented surface fills the whole window rect. The host draws the caption
        // bar and frame *around* that rect (see web_ui_host.js), exactly like the SDL backend
        // relies on the OS window manager to decorate the client area. We therefore forward
        // the guest's own (zero) insets unchanged rather than reserving title-bar space inside.
        ui_insets get_web_ui_client_insets(const ui_window_desc& desc)
        {
            return desc.client_insets;
        }

        std::mutex g_web_ui_event_mutex{};
        std::deque<queued_web_ui_event> g_web_ui_events{};
        bool g_web_ui_bridge_available = false;

        class web_ui_backend;
        web_ui_backend* g_active_web_ui_backend = nullptr;

        // clang-format off
        EM_JS(int, initialize_web_ui_bridge, (), {
            if (globalThis.__sogenUiBridgeInitialized) {
                return globalThis.__sogenUiBridgeAvailable ? 1 : 0;
            }

            globalThis.__sogenUiBridgeInitialized = true;
            globalThis.__sogenUiBridgeAvailable = false;

            if (typeof globalThis.addEventListener !== 'function' || typeof globalThis.postMessage !== 'function') {
                return 0;
            }

            globalThis.__sogenUiBridgeAvailable = true;

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
            return 1;
        });
        // clang-format on

        bool has_web_ui_bridge()
        {
            return g_web_ui_bridge_available;
        }

        void post_ui_message(const char* command, const hwnd window)
        {
            if (!has_web_ui_bridge())
            {
                return;
            }
            // clang-format off
            EM_ASM({
                const command = UTF8ToString($0);
                console.log('[sogen-ui][emit]', command, 'hwnd=' + Number($1 >>> 0)); // TEMP diagnostic
                postMessage({
                    type: 'sogen_ui',
                    command: command,
                    hwnd: Number($1 >>> 0),
                });
            }, command, static_cast<uint32_t>(window));
            // clang-format on
        }

        void post_ui_rect_message(const char* command, const hwnd window, const RECT& rect)
        {
            if (!has_web_ui_bridge())
            {
                return;
            }
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
            if (!has_web_ui_bridge())
            {
                return;
            }
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
            if (!has_web_ui_bridge())
            {
                return;
            }

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
            if (!has_web_ui_bridge())
            {
                return;
            }

            const auto class_name = u16_to_u8(desc.class_name);
            const auto title = u16_to_u8(desc.title);
            const auto client_insets = get_web_ui_client_insets(desc);
            // clang-format off
            EM_ASM({
                const rect = Number($3);
                const className = Number($4);
                const title = Number($5);
                const insets = Number($12);
                const rectIndex = rect / 4;
                const insetsIndex = insets / 4;
                postMessage({
                    type: 'sogen_ui',
                    command: 'create_window',
                    hwnd: Number($0 >>> 0),
                    parent: Number($1 >>> 0),
                    owner: Number($2 >>> 0),
                    rect: {
                        left: HEAP32[rectIndex + 0],
                        top: HEAP32[rectIndex + 1],
                        right: HEAP32[rectIndex + 2],
                        bottom: HEAP32[rectIndex + 3],
                    },
                    class_name: UTF8ToString(className),
                    title: UTF8ToString(title),
                    style: Number($6 >>> 0),
                    ex_style: Number($7 >>> 0),
                    control_id: Number($8 >>> 0),
                    visible: Boolean($9),
                    enabled: Boolean($10),
                    top_level: Boolean($11),
                    client_insets: {
                        left: HEAP32[insetsIndex + 0],
                        top: HEAP32[insetsIndex + 1],
                        right: HEAP32[insetsIndex + 2],
                        bottom: HEAP32[insetsIndex + 3],
                    },
                });
            }, static_cast<uint32_t>(desc.handle), static_cast<uint32_t>(desc.parent), static_cast<uint32_t>(desc.owner), &desc.rect,
               class_name.c_str(), title.c_str(), desc.style, desc.ex_style, desc.control_id, desc.visible ? 1 : 0,
               desc.enabled ? 1 : 0, desc.top_level ? 1 : 0, &client_insets);
            // clang-format on
        }

        void post_ui_present_surface(const hwnd window, const ui_surface_desc& surface)
        {
            if (!has_web_ui_bridge())
            {
                return;
            }

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
                console.log('[sogen-ui][emit] present_surface', 'hwnd=' + Number($0 >>> 0), $1 + 'x' + $2); // TEMP diagnostic
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
                g_web_ui_bridge_available = initialize_web_ui_bridge() != 0;
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
                // Drain, yield to the browser (which may enqueue more via the message bridge), then
                // drain again so freshly-arrived events are handled within this same pump call.
                this->drain_events();
                emscripten_sleep(0);
                this->drain_events();
            }

            void deliver_external_event(const ui_event& event)
            {
                // Always enqueue. Browser 'message' callbacks fire during emscripten_sleep(0), i.e. on the
                // asyncify-unwound stack mid-emulation; calling the sink (and emu().stop()) re-entrantly from
                // there is unsafe. pump_events drains the queue at a clean point, matching the SDL backend.
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
            void drain_events()
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
            }

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
