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

        // Keys whose text arrives separately as WM_CHAR; their key press is held until
        // the text is known.
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

        bool overlay_window(const window& value)
        {
            return (value.type & 0xFF) == 9 || (value.type & 0xFF) == 13;
        }

        uint32_t cursor_id(const int32_t shape)
        {
            switch (shape)
            {
            case 1:
                return 32516;
            case 2:
                return 32515;
            case 3:
                return 32514;
            case 4:
                return 32513;
            case 5:
            case 11:
                return 32645;
            case 6:
            case 12:
                return 32644;
            case 7:
                return 32643;
            case 8:
                return 32642;
            case 9:
            case 18:
                return 32646;
            case 13:
            case 17:
                return 32649;
            case 14:
                return 32648;
            case 16:
                return 32650;
            default:
                return 32512;
            }
        }

        auto stacking_order(const window& value)
        {
            return std::pair{value.raised, value.raised ? value.stacking_order : UINT32_MAX - value.stacking_order};
        }

        int64_t divide_up(const int64_t value, const int64_t divisor)
        {
            return value >= 0 ? (value + divisor - 1) / divisor : value / divisor;
        }

        void composite(const window& owner, const window& overlay, std::vector<uint8_t>& pixels)
        {
            const auto& base = *owner.surface;
            const auto& source = *overlay.surface;
            const int64_t dx = int64_t{overlay.geometry[0]} - owner.geometry[0];
            const int64_t dy = int64_t{overlay.geometry[1]} - owner.geometry[1];
            const auto left = std::clamp<int64_t>(divide_up(dx * base.width, owner.geometry[2]), 0, base.width);
            const auto top = std::clamp<int64_t>(divide_up(dy * base.height, owner.geometry[3]), 0, base.height);
            const auto right = std::clamp<int64_t>(divide_up((dx + overlay.geometry[2]) * base.width, owner.geometry[2]), 0, base.width);
            const auto bottom = std::clamp<int64_t>(divide_up((dy + overlay.geometry[3]) * base.height, owner.geometry[3]), 0, base.height);
            for (auto y = top; y < bottom; ++y)
            {
                const auto sy = (y * owner.geometry[3] - dy * base.height) * source.height / (int64_t{base.height} * overlay.geometry[3]);
                for (auto x = left; x < right; ++x)
                {
                    const auto sx = (x * owner.geometry[2] - dx * base.width) * source.width / (int64_t{base.width} * overlay.geometry[2]);
                    const auto* from = source.pixels->data() + sy * source.stride + sx * 4;
                    auto* to = pixels.data() + y * base.stride + x * 4;
                    const uint32_t inverse = 255u - from[3];
                    for (size_t channel = 0; channel < 4; ++channel)
                    {
                        to[channel] = static_cast<uint8_t>(std::min(255u, from[channel] + (to[channel] * inverse + 127u) / 255u));
                    }
                }
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
            window_key owner{};
            bool live{};
            bool enabled{true};
            bool dirty{};
            bool appeared{};
            bool hidden{};
            bool geometry_changed{};
            bool title_changed{};
            bool visibility_changed{};
            bool focus_changed{};
            bool raise_changed{};
            bool focus_restored{};
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
    } // namespace

    struct native_presenter::implementation
    {
        windows_emulator& emulator;
        ui_backend& ui;
        std::map<window_key, native_window> windows{};
        std::deque<queued_input> pending{};
        std::map<uint64_t, uint32_t> last_input{};
        std::set<window_key> active_hosts{};
        std::optional<window_key> cursor_window{};
        std::optional<std::pair<window_key, int32_t>> applied_cursor{};
        std::vector<uint8_t> composed{};

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

        bool visible(const window_key& key) const
        {
            const auto it = this->windows.find(key);
            return it != this->windows.end() && it->second.live && it->second.cached.visible;
        }

        bool descends(window_key child, const window_key& ancestor, const bool wire = false) const
        {
            for (size_t depth = 0; child.second && depth <= this->windows.size(); ++depth)
            {
                if (child == ancestor)
                {
                    return true;
                }
                const auto it = this->windows.find(child);
                if (it == this->windows.end())
                {
                    break;
                }
                child = wire ? window_key{child.first, it->second.cached.parent} : it->second.owner;
            }
            return false;
        }

        window_key parent_host(const window_key& key) const
        {
            auto parent = window_key{key.first, this->windows.at(key).cached.parent};
            for (size_t depth = 0; parent.second && depth <= this->windows.size(); ++depth)
            {
                const auto it = this->windows.find(parent);
                if (it == this->windows.end() || !it->second.live)
                {
                    break;
                }
                if (!overlay_window(it->second.cached))
                {
                    return parent;
                }
                parent = it->second.cached.parent ? window_key{parent.first, it->second.cached.parent} : it->second.owner;
            }
            return {};
        }

        window_key overlay_owner(const window_key& key) const
        {
            if (const auto parent = this->parent_host(key); parent.second)
            {
                return parent;
            }
            const auto usable = [&](const window_key& candidate) {
                const auto it = this->windows.find(candidate);
                return this->visible(candidate) && !overlay_window(it->second.cached) && !this->descends(candidate, key, true);
            };
            const auto previous = this->last_input.find(key.first);
            if (previous != this->last_input.end() && usable({key.first, previous->second}))
            {
                return {key.first, previous->second};
            }
            window_key result{};
            for (const auto& [candidate, state] : this->windows)
            {
                if (candidate.first == key.first && usable(candidate) &&
                    (!result.second ||
                     std::pair{state.cached.focus_order, state.cached.stacking_order} >
                         std::pair{this->windows.at(result).cached.focus_order, this->windows.at(result).cached.stacking_order}))
                {
                    result = candidate;
                }
            }
            return result;
        }

        window_key modal_for(const window_key& key) const
        {
            window_key result{};
            const auto& source = this->windows.at(key).cached;
            for (const auto& [candidate, state] : this->windows)
            {
                if (candidate.first != key.first || !this->visible(candidate) || overlay_window(state.cached) ||
                    state.cached.modality == 0 || this->descends(key, candidate) ||
                    (source.modality != 0 && source.stacking_order > state.cached.stacking_order))
                {
                    continue;
                }
                if ((state.cached.modality == 2 || (state.owner.second && this->descends(key, state.owner))) &&
                    (!result.second || state.cached.stacking_order > this->windows.at(result).cached.stacking_order))
                {
                    result = candidate;
                }
            }
            return result;
        }

        std::vector<window_key> overlays(const window_key& owner, const bool input_only = false) const
        {
            std::vector<window_key> result;
            for (const auto& [key, state] : this->windows)
            {
                if (state.owner == owner && this->visible(key) && overlay_window(state.cached) &&
                    (!input_only || (state.cached.type & 0xFF) == 9))
                {
                    result.push_back(key);
                }
            }
            std::ranges::sort(result, [&](const auto& left, const auto& right) {
                return stacking_order(this->windows.at(left).cached) < stacking_order(this->windows.at(right).cached);
            });
            return result;
        }

        void release_inputs(const window_key& key, native_window& state)
        {
            state.high_surrogate = 0;
            this->flush_key(key, state);
            for (const auto& [vk, code] : state.pressed)
            {
                (void)vk;
                this->key(key, qt_key_release, code, state.modifiers);
            }
            state.pressed.clear();
            for (const int32_t button : {1, 2, 4, 8, 16})
            {
                if ((state.buttons & button) != 0)
                {
                    state.buttons &= ~button;
                    this->queue(key, Mouse, {0, 0, state.buttons, button, qt_release, state.modifiers});
                }
            }
            state.modifiers = 0;
        }

        void activate(const window_key& key)
        {
            const auto found = this->windows.find(key);
            if (found != this->windows.end() && this->visible(key) && found->second.host && found->second.enabled)
            {
                this->ui.activate_window(found->second.host);
            }
        }

        window_key restore_target(native_window& state, const uint64_t session) const
        {
            if (state.focus_restored || !state.cached.focused || !state.owner.second || !this->visible(state.owner) ||
                std::ranges::none_of(this->active_hosts, [session](const auto& key) { return key.first == session; }))
            {
                return {};
            }
            for (const auto& [key, other] : this->windows)
            {
                if (key.first == session && other.live && other.cached.focus_order > state.cached.focus_order)
                {
                    return {};
                }
            }
            const auto modal = this->modal_for(state.owner);
            state.focus_restored = true;
            return modal.second ? modal : state.owner;
        }

        void restore_cursor()
        {
            if (this->applied_cursor)
            {
                this->emulator.apply_cursor_shape();
                this->ui.set_cursor_visibility(this->emulator.process.cursor_show_count >= 0 &&
                                               this->emulator.process.cursor_shape_visible);
                this->applied_cursor.reset();
            }
            this->cursor_window.reset();
        }

        void update_cursor()
        {
            if (!this->cursor_window || !this->visible(*this->cursor_window))
            {
                return;
            }
            auto target = *this->cursor_window;
            if (const auto modal = this->modal_for(target); modal.second)
            {
                target = modal;
            }
            const auto popups = this->overlays(target, true);
            if (!popups.empty())
            {
                target = popups.back();
            }
            const auto selection = std::pair{target, this->windows.at(target).cached.cursor};
            if (!this->applied_cursor || *this->applied_cursor != selection)
            {
                this->ui.set_system_cursor(cursor_id(selection.second));
                this->ui.set_cursor_visibility(selection.second != 10);
                this->applied_cursor = selection;
            }
        }

        void input(const window_key& host, const ui_event& event)
        {
            const auto found = this->windows.find(host);
            if (found == this->windows.end() || !this->visible(host) || !this->find_session(host.first))
            {
                return;
            }
            if (event.message == WM_SETFOCUS)
            {
                this->active_hosts.insert(host);
            }
            if (event.message == WM_KILLFOCUS)
            {
                this->active_hosts.erase(host);
                this->release_inputs(host, found->second);
                for (const auto& overlay : this->overlays(host))
                {
                    this->release_inputs(overlay, this->windows.at(overlay));
                }
                this->queue(host, Focus, {0});
                this->restore_cursor();
                return;
            }
            const bool keyboard = event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN || event.message == WM_KEYUP ||
                                  event.message == WM_SYSKEYUP || event.message == WM_CHAR;
            auto target = host;
            if (event.message != WM_SIZE)
            {
                const auto modal = this->modal_for(host);
                if (modal.second)
                {
                    this->activate(modal);
                    if (!keyboard)
                    {
                        if (event.message == WM_SETFOCUS || event.message == WM_LBUTTONDOWN || event.message == WM_RBUTTONDOWN ||
                            event.message == WM_MBUTTONDOWN || event.message == WM_XBUTTONDOWN)
                        {
                            this->queue(modal, Focus, {1});
                        }
                        return;
                    }
                    target = modal;
                }
                this->last_input[host.first] = target.second;
                if (event.message != WM_CLOSE)
                {
                    const auto popups = this->overlays(target, true);
                    if (!popups.empty())
                    {
                        if (event.message == WM_SETFOCUS)
                        {
                            return;
                        }
                        target = popups.back();
                    }
                }
            }
            if (event.message == WM_KEYUP || event.message == WM_SYSKEYUP)
            {
                for (const auto& [key, state] : this->windows)
                {
                    if ((key == host || state.owner == host) &&
                        (state.pressed.contains(event.wParam) || (state.pending && state.pending->vk == event.wParam)))
                    {
                        target = key;
                        break;
                    }
                }
            }
            auto& state = this->windows.at(target);
            if (target != host)
            {
                state.modifiers |= found->second.modifiers;
            }
            const auto& from = found->second.cached.geometry;
            const auto& to = state.cached.geometry;
            const auto x = static_cast<int32_t>(
                std::clamp<int64_t>(int64_t{static_cast<int16_t>(event.lParam & 0xFFFFu)} + from[0] - to[0], INT32_MIN, INT32_MAX));
            const auto y = static_cast<int32_t>(std::clamp<int64_t>(
                int64_t{static_cast<int16_t>((event.lParam >> 16u) & 0xFFFFu)} + from[1] - to[1], INT32_MIN, INT32_MAX));
            this->deliver_input(target, event, x, y);
            found->second.modifiers = state.modifiers;
            this->cursor_window = host;
            this->update_cursor();
        }

        void deliver_input(const window_key& id, const ui_event& event, const int32_t x, const int32_t y)
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
                this->release_inputs(id, state);
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

        size_t depth(window_key key) const
        {
            size_t count{};
            while (key.second && count <= this->windows.size())
            {
                const auto it = this->windows.find(key);
                if (it == this->windows.end())
                {
                    break;
                }
                key = it->second.owner;
                ++count;
            }
            return count;
        }

        void remove(const window_key& key)
        {
            const auto found = this->windows.find(key);
            if (auto owner = this->windows.find(found->second.owner); owner != this->windows.end())
            {
                owner->second.dirty = true;
            }
            found->second.subscription.reset();
            if (found->second.host)
            {
                this->ui.destroy_window(found->second.host);
            }
            if (this->cursor_window && *this->cursor_window == key)
            {
                this->restore_cursor();
            }
            this->active_hosts.erase(key);
            this->windows.erase(found);
        }

        void refresh_windows()
        {
            for (auto& [key, state] : this->windows)
            {
                (void)key;
                state.live = false;
            }
            for (auto& [index, device] : this->emulator.process.devices)
            {
                (void)index;
                auto* raster = device.get_internal_device<qt_raster_device>();
                if (!raster || !raster->session().active())
                {
                    continue;
                }
                const auto& session = raster->session();
                for (const auto& [id, value] : session.windows())
                {
                    const window_key key{session.identity(), id};
                    auto& state = this->windows[key];
                    const auto& old = state.cached;
                    state.appeared = value.visible && !old.visible;
                    state.hidden = !value.visible && old.visible;
                    state.geometry_changed = old.geometry != value.geometry;
                    state.title_changed = old.title != value.title;
                    state.visibility_changed = old.visible != value.visible;
                    state.focus_changed = old.focus_order != value.focus_order;
                    if (state.focus_changed)
                    {
                        state.focus_restored = false;
                    }
                    state.raise_changed = old.stacking_order != value.stacking_order;
                    const auto previous = old.surface ? old.surface->pixels : nullptr;
                    const auto current = value.surface ? value.surface->pixels : nullptr;
                    state.dirty = previous != current || state.geometry_changed || state.visibility_changed || state.raise_changed;
                    state.cached = value;
                    state.live = true;
                }
            }
            for (auto& [key, state] : this->windows)
            {
                if (state.live && overlay_window(state.cached) && state.cached.visible && (state.appeared || !state.owner.second))
                {
                    state.owner = this->overlay_owner(key);
                }
            }
            for (auto& [key, state] : this->windows)
            {
                if (state.live && !overlay_window(state.cached))
                {
                    state.owner = this->parent_host(key);
                }
            }
        }

        void refresh_hosts()
        {
            std::vector<window_key> hosts;
            for (const auto& [key, state] : this->windows)
            {
                if (state.live && !overlay_window(state.cached))
                {
                    hosts.push_back(key);
                }
            }
            std::ranges::sort(hosts, [&](const auto& left, const auto& right) { return this->depth(left) < this->depth(right); });
            for (const auto& key : hosts)
            {
                auto& state = this->windows.at(key);
                const auto& value = state.cached;
                const bool enabled = !this->modal_for(key).second;
                if (!state.host && value.visible)
                {
                    state.host = next_host_window();
                    state.subscription = this->emulator.subscribe_host_window_events(
                        state.host, [this, key](const ui_event& event) { this->input(key, event); });
                    ui_window_desc desc{};
                    desc.handle = state.host;
                    if (const auto parent = this->windows.find(state.owner); parent != this->windows.end())
                    {
                        desc.owner = parent->second.host;
                    }
                    desc.rect = rectangle(value);
                    desc.title = u8_to_u16(value.title);
                    desc.style = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE;
                    desc.visible = true;
                    desc.enabled = enabled;
                    desc.top_level = true;
                    this->ui.create_window(desc);
                    state.enabled = enabled;
                    state.dirty = true;
                }
                else if (state.host)
                {
                    if (state.geometry_changed)
                    {
                        this->ui.set_window_rect(state.host, rectangle(value));
                    }
                    if (state.title_changed)
                    {
                        this->ui.set_window_title(state.host, u8_to_u16(value.title));
                    }
                    if (state.visibility_changed)
                    {
                        this->ui.set_window_visible(state.host, value.visible);
                    }
                    if (state.enabled != enabled)
                    {
                        if (!enabled)
                        {
                            this->release_inputs(key, state);
                        }
                        this->ui.set_window_enabled(state.host, enabled);
                        state.enabled = enabled;
                    }
                }
            }
        }

        void present(const window_key& key, native_window& state)
        {
            if (!state.host || !state.cached.visible || !state.cached.surface || !state.dirty)
            {
                return;
            }
            const auto& frame = *state.cached.surface;
            const auto* pixels = frame.pixels->data();
            bool copied{};
            for (const auto& overlay : this->overlays(key))
            {
                const auto& value = this->windows.at(overlay).cached;
                if (!value.surface)
                {
                    continue;
                }
                if (!copied)
                {
                    this->composed.assign(frame.pixels->begin(), frame.pixels->end());
                    copied = true;
                }
                composite(state.cached, value, this->composed);
            }
            if (copied)
            {
                pixels = this->composed.data();
            }
            this->ui.present_surface(state.host, {.width = frame.width,
                                                  .height = frame.height,
                                                  .stride = frame.stride,
                                                  .format = ui_surface_format::bgra8,
                                                  .pixels = pixels});
        }

        void update()
        {
            this->refresh_windows();
            std::vector<window_key> removed;
            std::set<window_key> restored;
            for (auto& [key, state] : this->windows)
            {
                if (state.hidden)
                {
                    this->release_inputs(key, state);
                }
                if ((!state.live || state.hidden) && (overlay_window(state.cached) || state.cached.modality != 0))
                {
                    const auto target = this->restore_target(state, key.first);
                    if (target.second)
                    {
                        restored.insert(target);
                    }
                }
                if (!state.live)
                {
                    removed.push_back(key);
                }
                if (overlay_window(state.cached) && state.dirty)
                {
                    if (const auto owner = this->windows.find(state.owner); owner != this->windows.end())
                    {
                        owner->second.dirty = true;
                    }
                }
            }
            std::ranges::sort(removed, [&](const auto& left, const auto& right) { return this->depth(left) > this->depth(right); });
            for (const auto& key : removed)
            {
                this->remove(key);
            }
            std::erase_if(this->last_input, [&](const auto& item) {
                return std::ranges::none_of(this->windows, [&](const auto& entry) { return entry.first.first == item.first; });
            });
            this->refresh_hosts();
            for (auto& [key, state] : this->windows)
            {
                if (!overlay_window(state.cached) && state.cached.visible &&
                    ((state.focus_changed && state.cached.focused) || (state.raise_changed && state.cached.raised)))
                {
                    const auto modal = this->modal_for(key);
                    this->activate(modal.second ? modal : key);
                }
                this->present(key, state);
            }
            for (const auto& target : restored)
            {
                this->cursor_window = target;
                this->activate(target);
                this->queue(target, Focus, {1});
            }
            this->update_cursor();
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
            std::vector<window_key> keys;
            for (const auto& [key, state] : this->windows)
            {
                (void)state;
                keys.push_back(key);
            }
            std::ranges::sort(keys, [&](const auto& left, const auto& right) { return this->depth(left) > this->depth(right); });
            for (const auto& key : keys)
            {
                this->remove(key);
            }
            this->pending.clear();
            this->last_input.clear();
            this->active_hosts.clear();
            this->restore_cursor();
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

} // namespace sogen::qt_raster
