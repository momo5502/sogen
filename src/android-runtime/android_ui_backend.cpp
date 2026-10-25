#include "std_include.hpp"
#include "android_ui_backend.hpp"
#include "android_keymap.hpp"

#include <vulkan_handle_utils.hpp>

#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>

#include <dlfcn.h>

#ifdef SOGEN_HAS_ADRENOTOOLS
#include <adrenotools/driver.h>
#endif

namespace sogen::android
{
    namespace
    {
        constexpr auto surface_wait_timeout = std::chrono::seconds(10);

        // Touch keyboards deliver key down and up within the same frame. Games that poll the key state once
        // per frame would never see the key pressed, so releases are held back until the key was down this long.
        constexpr auto minimum_key_press_duration = std::chrono::milliseconds(50);

        uint64_t pack_point(const int32_t x, const int32_t y)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0xFFFFu) << 16) | (static_cast<uint32_t>(x) & 0xFFFFu);
        }

        int32_t client_width(const ui_window_desc& desc)
        {
            return std::max<int32_t>(1, (desc.rect.right - desc.rect.left) - desc.client_insets.left - desc.client_insets.right);
        }

        int32_t client_height(const ui_window_desc& desc)
        {
            return std::max<int32_t>(1, (desc.rect.bottom - desc.rect.top) - desc.client_insets.top - desc.client_insets.bottom);
        }

        uint16_t button_mask(const mouse_button button)
        {
            switch (button)
            {
            case mouse_button::right:
                return MK_RBUTTON;
            case mouse_button::middle:
                return MK_MBUTTON;
            default:
                return MK_LBUTTON;
            }
        }

        uint32_t button_message(const mouse_button button, const bool down)
        {
            switch (button)
            {
            case mouse_button::right:
                return down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            case mouse_button::middle:
                return down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            default:
                return down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            }
        }

        uint64_t make_key_lparam(const windows_key& key, const bool was_down, const bool key_up)
        {
            uint64_t value = 1;
            value |= static_cast<uint64_t>(key.scan_code) << 16;
            if (key.extended)
            {
                value |= 1ull << 24;
            }
            if (was_down)
            {
                value |= 1ull << 30;
            }
            if (key_up)
            {
                value |= 1ull << 31;
            }
            return value;
        }

        uint16_t control_character(const uint8_t virtual_key)
        {
            switch (virtual_key)
            {
            case VK_BACK:
                return 0x08;
            case VK_TAB:
                return 0x09;
            case VK_RETURN:
                return 0x0D;
            case VK_ESCAPE:
                return 0x1B;
            default:
                return 0;
            }
        }

        void copy_surface(const ANativeWindow_Buffer& buffer, const ui_surface_desc& surface)
        {
            const auto width = std::min(surface.width, buffer.width);
            const auto height = std::min(surface.height, buffer.height);
            const auto* source = static_cast<const uint8_t*>(surface.pixels);
            auto* destination = static_cast<uint8_t*>(buffer.bits);
            const auto destination_stride = static_cast<size_t>(buffer.stride) * 4;

            for (int32_t y = 0; y < height; ++y)
            {
                const auto* source_row = source + (static_cast<size_t>(y) * static_cast<size_t>(surface.stride));
                auto* destination_row = destination + (static_cast<size_t>(y) * destination_stride);

                for (int32_t x = 0; x < width; ++x)
                {
                    const auto* pixel = source_row + (static_cast<size_t>(x) * 4);
                    auto* target = destination_row + (static_cast<size_t>(x) * 4);
                    const bool swap = surface.format == ui_surface_format::bgra8;

                    target[0] = swap ? pixel[2] : pixel[0];
                    target[1] = pixel[1];
                    target[2] = swap ? pixel[0] : pixel[2];
                    // GDI leaves the alpha channel undefined; the texture is composited, so force it opaque.
                    target[3] = 0xFF;
                }
            }
        }

        // The library stays loaded for the life of the process; each session process runs a single game.
        void* load_vulkan_library(const vulkan_driver& driver)
        {
#ifdef SOGEN_HAS_ADRENOTOOLS
            // Turnip only drives Adreno GPUs, which Qualcomm's kernel driver exposes as kgsl.
            if (!driver.custom_name.empty() && std::filesystem::exists("/dev/kgsl-3d0"))
            {
                const auto hook_directory = driver.hook_directory + "/";
                const auto custom_directory = driver.custom_directory + "/";
                if (void* library = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_directory.c_str(),
                                                               custom_directory.c_str(), driver.custom_name.c_str(), nullptr, nullptr))
                {
                    return library;
                }
            }
#else
            (void)driver;
#endif

            return dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
    }

    android_ui_backend::android_ui_backend(ui_host& host, const vulkan_driver& driver)
        : host_(&host)
    {
        if (void* library = load_vulkan_library(driver))
        {
            this->vulkan_get_instance_proc_addr_ = dlsym(library, "vkGetInstanceProcAddr");
        }
    }

    android_ui_backend::~android_ui_backend()
    {
        this->release_surfaces();
    }

    void android_ui_backend::attach_surface(const hwnd window, ANativeWindow* surface)
    {
        const std::lock_guard lock{this->mutex_};

        const auto it = this->windows_.find(window);
        if (it == this->windows_.end())
        {
            ANativeWindow_release(surface);
            return;
        }

        if (it->second.surface)
        {
            ANativeWindow_release(it->second.surface);
        }

        it->second.surface = surface;
        this->surface_attached_.notify_all();
    }

    void android_ui_backend::detach_surface(const hwnd window)
    {
        const std::lock_guard lock{this->mutex_};

        if (const auto it = this->windows_.find(window); it != this->windows_.end() && it->second.surface)
        {
            ANativeWindow_release(it->second.surface);
            it->second.surface = nullptr;
        }
    }

    void android_ui_backend::post_mouse_move(const hwnd window, const int32_t x, const int32_t y)
    {
        const std::lock_guard lock{this->mutex_};
        this->queue_event(window, WM_MOUSEMOVE, this->mouse_buttons_, pack_point(x, y));
    }

    void android_ui_backend::post_mouse_motion(const hwnd window, const int32_t dx, const int32_t dy)
    {
        const std::lock_guard lock{this->mutex_};
        this->events_.push_back(ui_event{
            .window = window,
            .message = WM_MOUSEMOVE,
            .wParam = this->mouse_buttons_,
            .lParam = 0,
            .relative_motion = std::pair{dx, dy},
        });
    }

    void android_ui_backend::set_cursor_clip(const hwnd window, const std::optional<ui_rect>& /*screen_rect*/, const bool relative)
    {
        {
            const std::lock_guard lock{this->mutex_};
            this->clip_relative_window_ = relative ? this->top_level_ancestor(window) : 0;
        }

        this->update_relative_mouse();
    }

    void android_ui_backend::set_cursor_visibility(const bool visible)
    {
        {
            const std::lock_guard lock{this->mutex_};
            this->cursor_hidden_ = !visible;
        }

        this->update_relative_mouse();
    }

    void android_ui_backend::update_relative_mouse()
    {
        hwnd previous{};
        hwnd current{};

        {
            const std::lock_guard lock{this->mutex_};

            current = this->clip_relative_window_ != 0 ? this->clip_relative_window_ : (this->cursor_hidden_ ? this->active_window_ : 0);
            if (current == this->relative_window_)
            {
                return;
            }

            previous = std::exchange(this->relative_window_, current);
        }

        if (previous != 0)
        {
            this->host_->on_relative_mouse_changed(previous, false);
        }

        if (current != 0)
        {
            this->host_->on_relative_mouse_changed(current, true);
        }
    }

    void android_ui_backend::post_mouse_button(const hwnd window, const mouse_button button, const bool down, const int32_t x,
                                               const int32_t y)
    {
        const std::lock_guard lock{this->mutex_};

        this->activate(window);

        if (down)
        {
            this->mouse_buttons_ |= button_mask(button);
        }
        else
        {
            this->mouse_buttons_ &= static_cast<uint16_t>(~button_mask(button));
        }

        this->queue_event(window, WM_MOUSEMOVE, this->mouse_buttons_, pack_point(x, y));
        this->queue_event(window, button_message(button, down), this->mouse_buttons_, pack_point(x, y));
    }

    void android_ui_backend::post_mouse_wheel(const hwnd window, const int32_t delta, const int32_t x, const int32_t y)
    {
        const std::lock_guard lock{this->mutex_};

        const auto w_param = (static_cast<uint64_t>(static_cast<uint16_t>(delta)) << 16) | this->mouse_buttons_;
        this->queue_event(window, WM_MOUSEWHEEL, w_param, pack_point(x, y));
    }

    void android_ui_backend::post_key(const hwnd window, const int32_t keycode, const bool down)
    {
        const auto key = map_android_keycode(keycode);
        if (!key)
        {
            return;
        }

        const std::lock_guard lock{this->mutex_};

        this->activate(window);

        const bool system_key = key->virtual_key == VK_MENU || key->virtual_key == VK_F10;
        const uint32_t down_message = system_key ? WM_SYSKEYDOWN : WM_KEYDOWN;
        const uint32_t up_message = system_key ? WM_SYSKEYUP : WM_KEYUP;
        const auto now = std::chrono::steady_clock::now();
        const auto pressed = this->key_down_since_.find(key->virtual_key);
        const bool was_down = pressed != this->key_down_since_.end();

        if (down)
        {
            // A release that is still held back has to reach the guest before the key's next press.
            if (const auto held = std::ranges::find_if(this->delayed_events_,
                                                       [&](const delayed_event& delayed) {
                                                           return delayed.event.message == up_message &&
                                                                  delayed.event.wParam == key->virtual_key;
                                                       });
                held != this->delayed_events_.end())
            {
                this->events_.push_back(held->event);
                this->delayed_events_.erase(held);
            }

            if (!was_down)
            {
                this->key_down_since_.emplace(key->virtual_key, now);
            }

            const auto lparam = make_key_lparam(*key, was_down, false);
            this->queue_event(window, down_message, key->virtual_key, lparam);

            // Sogen's TranslateMessage does not synthesize WM_CHAR, so the backend delivers it. Printable characters
            // arrive through post_text, which knows the keyboard layout.
            if (const auto character = control_character(key->virtual_key); character != 0 && !system_key)
            {
                this->queue_event(window, WM_CHAR, character, lparam);
            }
            return;
        }

        const ui_event release{
            .window = window,
            .message = up_message,
            .wParam = key->virtual_key,
            .lParam = make_key_lparam(*key, true, true),
        };

        const auto due = was_down ? pressed->second + minimum_key_press_duration : now;
        if (was_down)
        {
            this->key_down_since_.erase(pressed);
        }

        if (due > now)
        {
            this->delayed_events_.push_back({.event = release, .due = due});
        }
        else
        {
            this->events_.push_back(release);
        }
    }

    void android_ui_backend::post_text(const hwnd window, const std::u16string_view text)
    {
        const std::lock_guard lock{this->mutex_};

        for (const auto character : text)
        {
            this->queue_event(window, WM_CHAR, character, 1);
        }
    }

    void android_ui_backend::post_close(const hwnd window)
    {
        const std::lock_guard lock{this->mutex_};
        this->queue_event(window, WM_CLOSE, 0, 0);
    }

    void android_ui_backend::set_event_sink(event_sink sink)
    {
        this->sink_ = std::move(sink);
    }

    void android_ui_backend::pump_events()
    {
        std::vector<ui_event> pending{};

        {
            const std::lock_guard lock{this->mutex_};
            pending.swap(this->events_);

            const auto now = std::chrono::steady_clock::now();
            std::erase_if(this->delayed_events_, [&](const delayed_event& delayed) {
                if (delayed.due > now)
                {
                    return false;
                }

                pending.push_back(delayed.event);
                return true;
            });
        }

        if (!this->sink_)
        {
            return;
        }

        for (const auto& event : pending)
        {
            this->sink_(event);
        }
    }

    void android_ui_backend::reset()
    {
        const std::lock_guard lock{this->mutex_};

        this->release_surfaces();
        this->windows_.clear();
        this->events_.clear();
        this->delayed_events_.clear();
        this->key_down_since_.clear();
        this->active_window_ = 0;
        this->mouse_buttons_ = 0;
    }

    void android_ui_backend::create_window(const ui_window_desc& desc)
    {
        {
            const std::lock_guard lock{this->mutex_};

            this->windows_[desc.handle].desc = desc;
            if (!desc.top_level)
            {
                return;
            }

            if (desc.visible && this->active_window_ == 0)
            {
                this->activate(desc.handle);
            }
        }

        this->host_->on_window_created(desc.handle, client_width(desc), client_height(desc), desc.title, desc.visible);
    }

    void android_ui_backend::destroy_window(const hwnd window)
    {
        bool top_level{};

        {
            const std::lock_guard lock{this->mutex_};

            const auto it = this->windows_.find(window);
            if (it == this->windows_.end())
            {
                return;
            }

            top_level = it->second.desc.top_level;
            if (it->second.surface)
            {
                ANativeWindow_release(it->second.surface);
            }

            this->windows_.erase(it);
            if (this->active_window_ == window)
            {
                this->active_window_ = 0;
            }

            if (this->relative_window_ == window)
            {
                this->relative_window_ = 0;
            }

            if (this->clip_relative_window_ == window)
            {
                this->clip_relative_window_ = 0;
            }
        }

        if (top_level)
        {
            this->host_->on_window_destroyed(window);
        }
    }

    void android_ui_backend::set_window_rect(const hwnd window, const RECT& rect)
    {
        ui_window_desc desc{};

        {
            const std::lock_guard lock{this->mutex_};

            const auto it = this->windows_.find(window);
            if (it == this->windows_.end())
            {
                return;
            }

            const auto previous_width = client_width(it->second.desc);
            const auto previous_height = client_height(it->second.desc);
            it->second.desc.rect = rect;
            desc = it->second.desc;

            if (!desc.top_level || (client_width(desc) == previous_width && client_height(desc) == previous_height))
            {
                return;
            }
        }

        this->host_->on_window_resized(window, client_width(desc), client_height(desc));
    }

    void android_ui_backend::set_cursor_position(const hwnd window, const int32_t screen_x, const int32_t screen_y)
    {
        hwnd top_level{};
        int32_t x{};
        int32_t y{};

        {
            const std::lock_guard lock{this->mutex_};

            const auto* state = this->find_top_level(window);
            if (state == nullptr)
            {
                return;
            }

            top_level = state->desc.handle;
            x = screen_x - (state->desc.rect.left + state->desc.client_insets.left);
            y = screen_y - (state->desc.rect.top + state->desc.client_insets.top);
        }

        this->host_->on_cursor_moved(top_level, x, y);
    }

    void android_ui_backend::set_window_visible(const hwnd window, const bool visible)
    {
        {
            const std::lock_guard lock{this->mutex_};

            const auto it = this->windows_.find(window);
            if (it == this->windows_.end())
            {
                return;
            }

            it->second.desc.visible = visible;
            if (!it->second.desc.top_level)
            {
                return;
            }

            if (visible)
            {
                this->activate(window);
            }
        }

        this->host_->on_window_visibility_changed(window, visible);
    }

    void android_ui_backend::set_window_title(const hwnd window, const std::u16string_view title)
    {
        {
            const std::lock_guard lock{this->mutex_};

            const auto it = this->windows_.find(window);
            if (it == this->windows_.end())
            {
                return;
            }

            it->second.desc.title = title;
            if (!it->second.desc.top_level)
            {
                return;
            }
        }

        this->host_->on_window_title_changed(window, title);
    }

    void android_ui_backend::present_surface(const hwnd window, const ui_surface_desc& surface)
    {
        if (!surface.pixels || surface.width <= 0 || surface.height <= 0)
        {
            return;
        }

        ANativeWindow* target{};

        {
            const std::lock_guard lock{this->mutex_};

            const auto* state = this->find_top_level(window);
            if (!state || !state->surface || state->vulkan_owned)
            {
                return;
            }

            target = state->surface;
            ANativeWindow_acquire(target);
        }

        if (ANativeWindow_setBuffersGeometry(target, surface.width, surface.height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) == 0)
        {
            ANativeWindow_Buffer buffer{};
            if (ANativeWindow_lock(target, &buffer, nullptr) == 0)
            {
                copy_surface(buffer, surface);
                ANativeWindow_unlockAndPost(target);
            }
        }

        ANativeWindow_release(target);
    }

    std::vector<std::string> android_ui_backend::vulkan_instance_extensions()
    {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    }

    uint64_t android_ui_backend::create_vulkan_surface(const hwnd window, const uint64_t instance)
    {
        const auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(this->vulkan_get_instance_proc_addr_);
        if (!get_instance_proc_addr)
        {
            return 0;
        }

        const auto vk_instance = unpack_vulkan_handle<VkInstance>(instance);
        const auto create_surface =
            reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(get_instance_proc_addr(vk_instance, "vkCreateAndroidSurfaceKHR"));
        if (!create_surface)
        {
            return 0;
        }

        ANativeWindow* target{};

        {
            std::unique_lock lock{this->mutex_};

            // The host creates the texture asynchronously after on_window_created, so the surface for a freshly
            // created window may not have arrived yet.
            this->surface_attached_.wait_for(lock, surface_wait_timeout, [&] {
                const auto* state = this->find_top_level(window);
                return !state || state->surface;
            });

            auto* state = this->find_top_level(window);
            if (!state || !state->surface)
            {
                return 0;
            }

            state->vulkan_owned = true;
            target = state->surface;
            ANativeWindow_acquire(target);
        }

        const VkAndroidSurfaceCreateInfoKHR create_info{
            .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .window = target,
        };

        VkSurfaceKHR surface{};
        const auto result = create_surface(vk_instance, &create_info, nullptr, &surface);
        ANativeWindow_release(target);

        return result == VK_SUCCESS ? pack_vulkan_handle(surface) : 0;
    }

    void* android_ui_backend::vulkan_get_instance_proc_addr()
    {
        return this->vulkan_get_instance_proc_addr_;
    }

    // An Android surface reports the size of the buffers its consumer was created with, which stays at the window's
    // initial size. Swapchains follow the guest window instead, and Flutter scales whatever buffer size arrives.
    std::optional<ui_extent> android_ui_backend::vulkan_surface_extent(const hwnd window)
    {
        const std::lock_guard lock{this->mutex_};

        const auto* state = this->find_top_level(window);
        if (!state)
        {
            return std::nullopt;
        }

        return ui_extent{
            .width = static_cast<uint32_t>(client_width(state->desc)),
            .height = static_cast<uint32_t>(client_height(state->desc)),
        };
    }

    void android_ui_backend::queue_event(const hwnd window, const uint32_t message, const uint64_t w_param, const uint64_t l_param)
    {
        this->events_.push_back(ui_event{.window = window, .message = message, .wParam = w_param, .lParam = l_param});
    }

    void android_ui_backend::activate(const hwnd window)
    {
        const auto top_level = this->top_level_ancestor(window);
        if (top_level == 0 || top_level == this->active_window_)
        {
            return;
        }

        if (this->active_window_ != 0)
        {
            this->queue_event(this->active_window_, WM_ACTIVATE, WA_INACTIVE, 0);
            this->queue_event(this->active_window_, WM_KILLFOCUS, 0, 0);
        }

        this->active_window_ = top_level;
        this->queue_event(top_level, WM_SETFOCUS, 0, 0);
        this->queue_event(top_level, WM_ACTIVATE, WA_ACTIVE, 0);
    }

    hwnd android_ui_backend::top_level_ancestor(hwnd window) const
    {
        while (true)
        {
            const auto it = this->windows_.find(window);
            if (it == this->windows_.end())
            {
                return 0;
            }

            if (it->second.desc.top_level || it->second.desc.parent == 0)
            {
                return window;
            }

            window = it->second.desc.parent;
        }
    }

    android_ui_backend::window_state* android_ui_backend::find_top_level(const hwnd window)
    {
        const auto it = this->windows_.find(this->top_level_ancestor(window));
        return it == this->windows_.end() ? nullptr : &it->second;
    }

    void android_ui_backend::release_surfaces()
    {
        for (auto& [handle, state] : this->windows_)
        {
            (void)handle;
            if (state.surface)
            {
                ANativeWindow_release(state.surface);
                state.surface = nullptr;
            }
        }
    }
}
