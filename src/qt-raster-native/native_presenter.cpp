#include "native_presenter.hpp"
#include <devices/qt_raster.hpp>

#include <atomic>
#include <set>

namespace sogen::qt_raster
{
    namespace
    {
        constexpr int32_t qt_press = 2, qt_release = 3, qt_move = 5, qt_key_press = 6, qt_key_release = 7;
        constexpr int32_t qt_shift = 0x02000000, qt_control = 0x04000000, qt_alt = 0x08000000, qt_keypad = 0x20000000;

        int32_t qt_key_for_vk(const uint64_t vk)
        {
            if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE)
            {
                return static_cast<int32_t>(vk);
            }
            if (vk >= 0x70 && vk <= 0x87)
            {
                return 0x01000030 + static_cast<int32_t>(vk - 0x70);
            }
            if (vk >= 0x60 && vk <= 0x69)
            {
                return '0' + static_cast<int32_t>(vk - 0x60);
            }
            switch (vk)
            {
            case VK_ESCAPE:
                return 0x01000000;
            case VK_TAB:
                return 0x01000001;
            case VK_BACK:
                return 0x01000003;
            case VK_RETURN:
                return 0x01000004;
            case 0x2D:
                return 0x01000006;
            case VK_DELETE:
                return 0x01000007;
            case 0x13:
                return 0x01000008;
            case 0x2C:
                return 0x01000009;
            case VK_HOME:
                return 0x01000010;
            case VK_END:
                return 0x01000011;
            case VK_LEFT:
                return 0x01000012;
            case VK_UP:
                return 0x01000013;
            case VK_RIGHT:
                return 0x01000014;
            case VK_DOWN:
                return 0x01000015;
            case 0x21:
                return 0x01000016;
            case 0x22:
                return 0x01000017;
            case VK_SHIFT:
            case 0xA0:
            case 0xA1:
                return 0x01000020;
            case VK_CONTROL:
            case 0xA2:
            case 0xA3:
                return 0x01000021;
            case VK_MENU:
            case 0xA4:
            case 0xA5:
                return 0x01000023;
            case 0x14:
                return 0x01000024;
            case 0x90:
                return 0x01000025;
            case 0x91:
                return 0x01000026;
            case 0x5D:
                return 0x01000055;
            case 0x6A:
                return '*';
            case 0x6B:
                return '+';
            case 0x6D:
                return '-';
            case 0x6E:
                return '.';
            case 0x6F:
                return '/';
            case 0xBA:
                return ';';
            case 0xBB:
                return '=';
            case 0xBC:
                return ',';
            case 0xBD:
                return '-';
            case 0xBE:
                return '.';
            case 0xBF:
                return '/';
            case 0xC0:
                return '`';
            case 0xDB:
                return '[';
            case 0xDC:
                return '\\';
            case 0xDD:
                return ']';
            case 0xDE:
                return '\'';
            default:
                return 0;
            }
        }

        bool is_keypad_vk(const uint64_t vk)
        {
            return vk >= 0x60 && vk <= 0x6F;
        }

        // Keys whose text arrives separately as WM_CHAR; their key press is held until the text is known.
        bool produces_text(const uint64_t vk)
        {
            return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE || (vk >= 0x60 && vk <= 0x6F && vk != 0x6C) ||
                   (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDE);
        }

        int32_t modifier_flag(const uint64_t vk)
        {
            switch (vk)
            {
            case VK_SHIFT:
            case 0xA0:
            case 0xA1:
                return qt_shift;
            case VK_CONTROL:
            case 0xA2:
            case 0xA3:
                return qt_control;
            case VK_MENU:
            case 0xA4:
            case 0xA5:
                return qt_alt;
            default:
                return 0;
            }
        }

        const char* text_for_vk(const uint64_t vk)
        {
            switch (vk)
            {
            case VK_ESCAPE:
                return "\x1b";
            case VK_TAB:
                return "\t";
            case VK_BACK:
                return "\b";
            case VK_RETURN:
                return "\r";
            case VK_DELETE:
                return "\x7f";
            default:
                return "";
            }
        }

        using window_key = std::pair<uint64_t, uint32_t>;

        struct pending_key
        {
            uint64_t vk{};
            int32_t key{};
            int32_t modifiers{};
            bool repeat{};
        };

        struct native_window
        {
            hwnd host{};
            window cached{};
            ui_event_router::subscription subscription{};
            std::optional<pending_key> pending{};
            uint16_t high_surrogate{};
            std::map<uint64_t, int32_t> pressed{};
            int32_t modifiers{};
            int32_t buttons{};
        };

        struct queued_input
        {
            Packet packet{};
            std::vector<uint8_t> payload{};
        };

        hwnd next_host_window()
        {
            static std::atomic<uint64_t> next{1};
            const auto value = next.fetch_add(1);
            if (value >= UINT32_MAX)
            {
                throw std::runtime_error("Native raster window IDs exhausted");
            }
            return 0x5142000000000000ULL | value;
        }

        RECT rectangle(const window& value)
        {
            return {.left = value.geometry[0],
                    .top = value.geometry[1],
                    .right = value.geometry[0] + value.geometry[2],
                    .bottom = value.geometry[1] + value.geometry[3]};
        }
    }

    struct native_presenter::implementation
    {
        windows_emulator& emulator;
        ui_backend& ui;
        std::map<window_key, native_window> windows{};
        std::deque<queued_input> pending{};

        implementation(windows_emulator& emu, ui_backend& backend)
            : emulator(emu),
              ui(backend)
        {
        }

        session* find_session(const uint64_t identity)
        {
            for (auto& [index, device] : this->emulator.process.devices)
            {
                (void)index;
                auto* raster = device.get_internal_device<qt_raster_device>();
                if (raster && raster->session().active() && raster->session().identity() == identity)
                {
                    return &raster->session();
                }
            }
            return nullptr;
        }

        void queue(const window_key& key, const Op op, const std::array<int32_t, 8>& arguments = {}, const std::string_view text = {})
        {
            Packet packet{};
            packet.op = op;
            packet.id = key.second;
            packet.session = key.first;
            packet.a = arguments;
            packet.size = static_cast<uint32_t>(text.size());
            if (op == Mouse && arguments[4] == qt_move && !this->pending.empty())
            {
                auto& last = this->pending.back();
                if (last.packet.op == Mouse && last.packet.a[4] == qt_move && last.packet.id == packet.id &&
                    last.packet.session == packet.session && last.packet.a[2] == packet.a[2] && last.packet.a[5] == packet.a[5])
                {
                    last.packet = packet;
                    return;
                }
            }
            if (this->pending.size() >= 4096)
            {
                throw std::runtime_error("Native raster input backlog exhausted");
            }
            this->pending.push_back({.packet = packet, .payload = {text.begin(), text.end()}});
        }

        void key(const window_key& id, const int32_t action, const int32_t code, const int32_t modifiers, const std::string_view text = {},
                 const bool repeat = false)
        {
            this->queue(id, Key, {action, code, modifiers, repeat ? 1 : 0}, text);
        }

        void flush_key(const window_key& id, native_window& state)
        {
            if (state.pending)
            {
                const auto key = *state.pending;
                state.pending.reset();
                state.pressed[key.vk] = key.key;
                this->key(id, qt_key_press, key.key, key.modifiers, {}, key.repeat);
            }
        }

        void input(const window_key& id, const ui_event& event)
        {
            const auto found = this->windows.find(id);
            if (found == this->windows.end() || !this->find_session(id.first))
            {
                return;
            }
            auto& state = found->second;
            if (event.message != WM_CHAR)
            {
                state.high_surrogate = 0;
                this->flush_key(id, state);
            }
            const auto x = static_cast<int16_t>(event.lParam & 0xFFFFu);
            const auto y = static_cast<int16_t>((event.lParam >> 16u) & 0xFFFFu);
            const auto mouse = [&](const int32_t action, const int32_t changed) {
                this->queue(id, Mouse, {x, y, state.buttons, changed, action, state.modifiers});
            };
            switch (event.message)
            {
            case WM_MOUSEMOVE:
                mouse(qt_move, 0);
                break;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP: {
                const auto down = event.message == WM_LBUTTONDOWN || event.message == WM_RBUTTONDOWN || event.message == WM_MBUTTONDOWN ||
                                  event.message == WM_XBUTTONDOWN;
                int32_t button = 4;
                if (event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONUP)
                {
                    button = 1;
                }
                else if (event.message == WM_RBUTTONDOWN || event.message == WM_RBUTTONUP)
                {
                    button = 2;
                }
                else if (event.message == WM_XBUTTONDOWN || event.message == WM_XBUTTONUP)
                {
                    const auto extra = (event.wParam >> 16u) & 0xFFFFu;
                    if (extra != 1 && extra != 2)
                    {
                        break;
                    }
                    button = extra == 1 ? 8 : 16;
                }
                state.buttons = down ? state.buttons | button : state.buttons & ~button;
                mouse(down ? qt_press : qt_release, button);
                break;
            }
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                const auto delta = static_cast<int16_t>((event.wParam >> 16u) & 0xFFFFu);
                const bool horizontal = event.message == WM_MOUSEHWHEEL;
                // Horizontal wheel polarity is opposite in Qt and WM_MOUSEHWHEEL.
                this->queue(id, Wheel, {x, y, horizontal ? -delta : 0, horizontal ? 0 : delta, state.modifiers});
                break;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                state.modifiers |= modifier_flag(event.wParam);
                const auto code = qt_key_for_vk(event.wParam);
                if (code == 0)
                {
                    break;
                }
                const auto modifiers = state.modifiers | (is_keypad_vk(event.wParam) ? qt_keypad : 0);
                const bool repeat = (event.lParam & (uint64_t{1} << 30u)) != 0;
                if (produces_text(event.wParam) && (state.modifiers & (qt_control | qt_alt)) == 0)
                {
                    state.pending = pending_key{.vk = event.wParam, .key = code, .modifiers = modifiers, .repeat = repeat};
                }
                else
                {
                    state.pressed[event.wParam] = code;
                    this->key(id, qt_key_press, code, modifiers, text_for_vk(event.wParam), repeat);
                }
                break;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const auto found_key = state.pressed.find(event.wParam);
                if (found_key != state.pressed.end())
                {
                    this->key(id, qt_key_release, found_key->second, state.modifiers | (is_keypad_vk(event.wParam) ? qt_keypad : 0));
                    state.pressed.erase(found_key);
                }
                state.modifiers &= ~modifier_flag(event.wParam);
                break;
            }
            case WM_CHAR: {
                auto cp = static_cast<uint32_t>(event.wParam);
                const auto high = std::exchange(state.high_surrogate, uint16_t{});
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    state.high_surrogate = static_cast<uint16_t>(cp);
                    break;
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    if (high == 0)
                    {
                        this->flush_key(id, state);
                        break;
                    }
                    cp = 0x10000 + ((uint32_t{high} - 0xD800) << 10u) + (cp - 0xDC00);
                }
                if (cp < 0x20 || cp == 0x7F || cp > 0x10FFFF)
                {
                    this->flush_key(id, state);
                    break;
                }
                const auto code = static_cast<int32_t>(cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp);
                const auto character = static_cast<char32_t>(cp);
                const auto text = u32_to_u8({&character, 1});
                if (state.pending)
                {
                    const auto key = *state.pending;
                    state.pending.reset();
                    state.pressed[key.vk] = code;
                    this->key(id, qt_key_press, code, key.modifiers, text, key.repeat);
                }
                else
                {
                    this->key(id, qt_key_press, code, state.modifiers, text);
                    this->key(id, qt_key_release, code, state.modifiers);
                }
                break;
            }
            case WM_SETFOCUS:
                this->queue(id, Focus, {1});
                break;
            case WM_KILLFOCUS:
                for (const auto& [vk, code] : state.pressed)
                {
                    (void)vk;
                    this->key(id, qt_key_release, code, state.modifiers);
                }
                state.pressed.clear();
                for (const int32_t button : {1, 2, 4, 8, 16})
                {
                    if ((state.buttons & button) != 0)
                    {
                        state.buttons &= ~button;
                        mouse(qt_release, button);
                    }
                }
                state.modifiers = 0;
                this->queue(id, Focus, {0});
                break;
            case WM_CLOSE:
                this->queue(id, Close);
                break;
            case WM_SIZE: {
                const auto width = static_cast<int32_t>(event.lParam & 0xFFFFu);
                const auto height = static_cast<int32_t>((event.lParam >> 16u) & 0xFFFFu);
                if (width > 0 && height > 0 && (width != state.cached.geometry[2] || height != state.cached.geometry[3]))
                {
                    this->queue(id, Resize, {state.cached.geometry[0], state.cached.geometry[1], width, height});
                }
                break;
            }
            default:
                break;
            }
        }

        void remove(const std::map<window_key, native_window>::iterator it)
        {
            it->second.subscription.reset();
            this->ui.destroy_window(it->second.host);
            this->windows.erase(it);
        }

        void update()
        {
            std::set<window_key> live{};
            for (auto& [index, device] : this->emulator.process.devices)
            {
                (void)index;
                auto* raster = device.get_internal_device<qt_raster_device>();
                if (!raster || !raster->session().active())
                {
                    continue;
                }
                const auto& session = raster->session();
                for (const auto& [id, window] : session.windows())
                {
                    const window_key key{session.identity(), id};
                    live.insert(key);
                    auto it = this->windows.find(key);
                    if (it == this->windows.end())
                    {
                        if (!window.visible)
                        {
                            continue;
                        }
                        const auto host = next_host_window();
                        auto subscription = this->emulator.subscribe_host_window_events(
                            host, [this, key](const ui_event& event) { this->input(key, event); });
                        ui_window_desc desc{};
                        desc.handle = host;
                        desc.rect = rectangle(window);
                        desc.title = u8_to_u16(window.title);
                        desc.style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE;
                        desc.visible = true;
                        desc.top_level = true;
                        it = this->windows.try_emplace(key).first;
                        it->second.host = host;
                        it->second.subscription = std::move(subscription);
                        this->ui.create_window(desc);
                        it->second.cached = window;
                        it->second.cached.surface.reset();
                    }
                    auto& state = it->second;
                    if (state.cached.geometry != window.geometry)
                    {
                        this->ui.set_window_rect(state.host, rectangle(window));
                    }
                    if (state.cached.title != window.title)
                    {
                        this->ui.set_window_title(state.host, u8_to_u16(window.title));
                    }
                    if (state.cached.visible != window.visible)
                    {
                        this->ui.set_window_visible(state.host, window.visible);
                    }
                    if (window.surface && (!state.cached.surface || state.cached.surface->pixels != window.surface->pixels))
                    {
                        const auto& frame = *window.surface;
                        this->ui.present_surface(state.host, {.width = frame.width,
                                                              .height = frame.height,
                                                              .stride = frame.stride,
                                                              .format = ui_surface_format::bgra8,
                                                              .pixels = frame.pixels->data()});
                    }
                    state.cached = window;
                }
            }
            for (auto it = this->windows.begin(); it != this->windows.end();)
            {
                if (!live.contains(it->first))
                {
                    this->remove(it++);
                }
                else
                {
                    ++it;
                }
            }
            while (!this->pending.empty())
            {
                const auto& input = this->pending.front();
                auto* session = this->find_session(input.packet.session);
                if (!session || !session->windows().contains(input.packet.id))
                {
                    this->pending.pop_front();
                    continue;
                }
                if (!session->enqueue_input(input.packet, input.payload))
                {
                    break;
                }
                this->pending.pop_front();
            }
        }

        void clear()
        {
            while (!this->windows.empty())
            {
                this->remove(this->windows.begin());
            }
            this->pending.clear();
        }
    };

    native_presenter::native_presenter(windows_emulator& emulator, ui_backend& ui)
        : implementation_(std::make_unique<implementation>(emulator, ui))
    {
    }

    native_presenter::~native_presenter()
    {
        this->clear();
    }

    void native_presenter::update()
    {
        this->implementation_->update();
    }

    void native_presenter::clear()
    {
        this->implementation_->clear();
    }

}
