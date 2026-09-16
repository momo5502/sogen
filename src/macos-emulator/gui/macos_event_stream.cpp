#include "../std_include.hpp"
#include "macos_event_stream.hpp"

#include "../mach/mach_msg.hpp"
#include "../mach/mig_kernel_servers.hpp"
#include "../macos_emulator.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>

namespace sogen
{
    namespace
    {
        // A field id carries its own payload shape: 0x1000 means a byte blob, 0x4000 a 32-bit element,
 // and neither a 64-bit one.
        constexpr uint16_t FIELD_VERSION = 0x4035;
        constexpr uint16_t FIELD_SUBVERSION = 0x4036;
        constexpr uint16_t FIELD_TYPE = 0x4037;
        constexpr uint16_t FIELD_GLOBAL_POINT = 0xc038;
        constexpr uint16_t FIELD_WINDOW_POINT = 0xc039;
        constexpr uint16_t FIELD_TIMESTAMP = 0x003a;
        constexpr uint16_t FIELD_FLAGS = 0x403b;
        constexpr uint16_t FIELD_WINDOW = 0x4033;
        constexpr uint16_t FIELD_CONNECTION = 0x4034;
        constexpr uint16_t FIELD_POSTER_PID = 0x4029;
        constexpr uint16_t FIELD_COALESCED = 0x402d;
        constexpr uint16_t FIELD_CONNECTION_AGAIN = 0x4055;
        constexpr uint16_t FIELD_WINDOW_ORIGIN_Y = 0x4069;
        constexpr uint16_t FIELD_WINDOW_HEIGHT = 0x406a;
        constexpr uint16_t FIELD_SCREEN_HEIGHT = 0x406b;

        constexpr uint16_t FIELD_BUTTON_DOWN = 0x4001;
        constexpr uint16_t FIELD_PRESSURE = 0x4002;
        constexpr uint16_t FIELD_BUTTON_NUMBER = 0x4003;
        constexpr uint16_t FIELD_EVENT_WINDOW_A = 0x405b;
        constexpr uint16_t FIELD_EVENT_WINDOW_B = 0x405c;
        constexpr uint16_t FIELD_CLICK_COUNT = 0x406c;

        constexpr uint16_t FIELD_SCROLL_LINES_1 = 0x400b;
        constexpr uint16_t FIELD_SCROLL_LINES_2 = 0x400c;
        constexpr uint16_t FIELD_SCROLL_LINES_3 = 0x400d;
        constexpr uint16_t FIELD_SCROLL_FIXED_1 = 0x405d;
        constexpr uint16_t FIELD_SCROLL_FIXED_2 = 0x405e;
        constexpr uint16_t FIELD_SCROLL_FIXED_3 = 0x405f;
        constexpr uint16_t FIELD_SCROLL_POINTS_1 = 0x4060;
        constexpr uint16_t FIELD_SCROLL_POINTS_2 = 0x4061;
        constexpr uint16_t FIELD_SCROLL_POINTS_3 = 0x4062;

        constexpr uint16_t FIELD_KEYCODE = 0x4009;
        constexpr uint16_t FIELD_KEY_ZERO = 0x4008;
        constexpr uint16_t FIELD_KEYBOARD_ID = 0x400a;
        constexpr uint16_t FIELD_KEY_CONSTANT = 0x404c;
        constexpr uint16_t FIELD_CHARACTER = 0x404d;
        constexpr uint16_t FIELD_CHARACTER_UNMODIFIED = 0x404e;
        constexpr uint16_t FIELD_KEY_SUBTYPE = 0x4050;
        constexpr uint16_t FIELD_CHARACTER_AGAIN = 0x4052;

        // Measured 25G76: every key record carries 0xfc here and 10 in the subtype, on key-up as well
        // as on key-down.
        constexpr uint32_t KEY_CONSTANT = 0xfc;
        constexpr uint32_t KEY_SUBTYPE = 10;

        constexpr uint32_t MOUSE_PRESSURE_DOWN = 255;

        // A line of scroll is ten points on this build, from the pair (3, -4) lines arriving as
        // (30, -40) points.
        constexpr int32_t SCROLL_POINTS_PER_LINE = 10;

        // Measured bits of the datagram-available ping: local disposition move_send, a voucher
        // disposition of the same value, and MACH_MSGH_BITS_RAISEIMPACT.
        constexpr uint32_t PING_BITS = 0x20111100;

        class tlv_writer
        {
          public:
            void u32(const uint16_t field, const uint32_t value)
            {
                this->tag(field, 1);
                this->raw32(value);
            }

            void u64(const uint16_t field, const uint64_t value)
            {
                this->tag(field, 1);
                for (uint32_t shift = 0; shift < 64; shift += 8)
                {
                    this->bytes_.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
                }
            }

            // A CGPoint travels as two big-endian f32 in two 32-bit elements, which is why the count is
            // two and the bytes are not written little-endian like everything else.
            void point(const uint16_t field, const float x, const float y)
            {
                this->tag(field, 2);
                this->big_endian_float(x);
                this->big_endian_float(y);
            }

            std::vector<uint8_t> take()
            {
                return std::move(this->bytes_);
            }

          private:
            void tag(const uint16_t field, const uint16_t count)
            {
                this->raw32(static_cast<uint32_t>(field) | (static_cast<uint32_t>(count) << 16));
            }

            void raw32(const uint32_t value)
            {
                for (uint32_t shift = 0; shift < 32; shift += 8)
                {
                    this->bytes_.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
                }
            }

            void big_endian_float(const float value)
            {
                uint32_t raw = 0;
                std::memcpy(&raw, &value, sizeof(raw));
                for (int32_t shift = 24; shift >= 0; shift -= 8)
                {
                    this->bytes_.push_back(static_cast<uint8_t>((raw >> shift) & 0xFFu));
                }
            }

            std::vector<uint8_t> bytes_{};
        };

        bool is_mouse_button(const macos_event_kind kind)
        {
            switch (kind)
            {
            case macos_event_kind::left_mouse_down:
            case macos_event_kind::left_mouse_up:
            case macos_event_kind::right_mouse_down:
            case macos_event_kind::right_mouse_up:
            case macos_event_kind::other_mouse_down:
            case macos_event_kind::other_mouse_up:
                return true;
            default:
                return false;
            }
        }

        bool is_key(const macos_event_kind kind)
        {
            return kind == macos_event_kind::key_down || kind == macos_event_kind::key_up;
        }

        std::map<macos_emulator*, macos_event_stream*>& installed_streams()
        {
            static std::map<macos_emulator*, macos_event_stream*> streams{};
            return streams;
        }

        uint32_t macos_keycode_for_virtual_key(const uint64_t vk)
        {
            // kHIDUsage/kVK_ANSI_* from Carbon's Events.h. A US layout: sogen has no keyboard-layout
            // model, and a wrong layout would be a silent mistranslation rather than a missing feature.
            switch (vk)
            {
            case 'A':
                return 0;
            case 'S':
                return 1;
            case 'D':
                return 2;
            case 'F':
                return 3;
            case 'H':
                return 4;
            case 'G':
                return 5;
            case 'Z':
                return 6;
            case 'X':
                return 7;
            case 'C':
                return 8;
            case 'V':
                return 9;
            case 'B':
                return 11;
            case 'Q':
                return 12;
            case 'W':
                return 13;
            case 'E':
                return 14;
            case 'R':
                return 15;
            case 'Y':
                return 16;
            case 'T':
                return 17;
            case '1':
                return 18;
            case '2':
                return 19;
            case '3':
                return 20;
            case '4':
                return 21;
            case '6':
                return 22;
            case '5':
                return 23;
            case VK_OEM_PLUS:
                return 24;
            case '9':
                return 25;
            case '7':
                return 26;
            case VK_OEM_MINUS:
                return 27;
            case '8':
                return 28;
            case '0':
                return 29;
            case VK_OEM_6:
                return 30;
            case 'O':
                return 31;
            case 'U':
                return 32;
            case VK_OEM_4:
                return 33;
            case 'I':
                return 34;
            case 'P':
                return 35;
            case VK_RETURN:
                return 36;
            case 'L':
                return 37;
            case 'J':
                return 38;
            case VK_OEM_7:
                return 39;
            case 'K':
                return 40;
            case VK_OEM_1:
                return 41;
            case VK_OEM_5:
                return 42;
            case VK_OEM_COMMA:
                return 43;
            case VK_OEM_2:
                return 44;
            case 'N':
                return 45;
            case 'M':
                return 46;
            case VK_OEM_PERIOD:
                return 47;
            case VK_TAB:
                return 48;
            case VK_SPACE:
                return 49;
            case VK_OEM_3:
                return 50;
            case VK_BACK:
                return 51;
            case VK_ESCAPE:
                return 53;
            case VK_LWIN:
            case VK_RWIN:
                return 55;
            case VK_SHIFT:
                return 56;
            case VK_CAPITAL:
                return 57;
            case VK_MENU:
                return 58;
            case VK_CONTROL:
                return 59;
            case VK_DECIMAL:
                return 65;
            case VK_MULTIPLY:
                return 67;
            case VK_ADD:
                return 69;
            case VK_DIVIDE:
                return 75;
            case VK_SUBTRACT:
                return 78;
            case VK_NUMPAD0:
                return 82;
            case VK_NUMPAD1:
                return 83;
            case VK_NUMPAD2:
                return 84;
            case VK_NUMPAD3:
                return 85;
            case VK_NUMPAD4:
                return 86;
            case VK_NUMPAD5:
                return 87;
            case VK_NUMPAD6:
                return 88;
            case VK_NUMPAD7:
                return 89;
            case VK_NUMPAD8:
                return 91;
            case VK_NUMPAD9:
                return 92;
            case VK_F5:
                return 96;
            case VK_F6:
                return 97;
            case VK_F7:
                return 98;
            case VK_F3:
                return 99;
            case VK_F8:
                return 100;
            case VK_F9:
                return 101;
            case VK_F11:
                return 103;
            case VK_F10:
                return 109;
            case VK_F12:
                return 111;
            case VK_HOME:
                return 115;
            case VK_PRIOR:
                return 116;
            case VK_DELETE:
                return 117;
            case VK_F4:
                return 118;
            case VK_END:
                return 119;
            case VK_F2:
                return 120;
            case VK_NEXT:
                return 121;
            case VK_F1:
                return 122;
            case VK_LEFT:
                return 123;
            case VK_RIGHT:
                return 124;
            case VK_DOWN:
                return 125;
            case VK_UP:
                return 126;
            default:
                return UINT32_MAX;
            }
        }

        uint32_t character_for_virtual_key(const uint64_t vk, const bool shifted)
        {
            if (vk >= 'A' && vk <= 'Z')
            {
                return static_cast<uint32_t>(shifted ? vk : vk + ('a' - 'A'));
            }

            if (vk >= '0' && vk <= '9')
            {
                constexpr std::string_view shifted_digits = ")!@#$%^&*(";
                return shifted ? static_cast<uint32_t>(static_cast<unsigned char>(shifted_digits[vk - '0'])) : static_cast<uint32_t>(vk);
            }

            switch (vk)
            {
            case VK_SPACE:
                return ' ';
            case VK_RETURN:
                return '\r';
            case VK_TAB:
                return '\t';
            case VK_BACK:
                return 0x08;
            case VK_ESCAPE:
                return 0x1b;
            case VK_OEM_1:
                return shifted ? ':' : ';';
            case VK_OEM_PLUS:
                return shifted ? '+' : '=';
            case VK_OEM_COMMA:
                return shifted ? '<' : ',';
            case VK_OEM_MINUS:
                return shifted ? '_' : '-';
            case VK_OEM_PERIOD:
                return shifted ? '>' : '.';
            case VK_OEM_2:
                return shifted ? '?' : '/';
            case VK_OEM_3:
                return shifted ? '~' : '`';
            case VK_OEM_4:
                return shifted ? '{' : '[';
            case VK_OEM_5:
                return shifted ? '|' : '\\';
            case VK_OEM_6:
                return shifted ? '}' : ']';
            case VK_OEM_7:
                return shifted ? '"' : '\'';
            default:
                return 0;
            }
        }

        uint32_t modifier_bit_for_virtual_key(const uint64_t vk)
        {
            switch (vk)
            {
            case VK_SHIFT:
                return MACOS_CG_FLAG_SHIFT;
            case VK_CONTROL:
                return MACOS_CG_FLAG_CONTROL;
            case VK_MENU:
                return MACOS_CG_FLAG_ALTERNATE;
            case VK_LWIN:
            case VK_RWIN:
                return MACOS_CG_FLAG_COMMAND;
            default:
                return 0;
            }
        }

        // The record's timestamp is nanoseconds since boot. It has to come from the clock the guest
        // itself reads, or a record would carry a time the guest's own mach_absolute_time never reaches:
        // CNTVCT_EL0 scaled by the timebase the emulator publishes.
        uint64_t event_timestamp(macos_emulator& emu)
        {
            const auto ticks = emu.emu().read_system_register(3, 3, 14, 0, 2);
            const auto denom = emu.mach.timebase_denom == 0 ? 1u : emu.mach.timebase_denom;
            return ticks * emu.mach.timebase_numer / denom;
        }

        bool is_mouse_down(const macos_event_kind kind)
        {
            switch (kind)
            {
            case macos_event_kind::left_mouse_down:
            case macos_event_kind::right_mouse_down:
            case macos_event_kind::other_mouse_down:
                return true;
            default:
                return false;
            }
        }

        bool post_and_track(macos_emulator& emu, macos_event_stream& events, const macos_event_record& record)
        {
            if (!is_key(record.kind))
            {
                macos_update_scoreboard_cursor(emu, record.global_x, record.global_y, record.timestamp);
            }

            return events.post(emu, record);
        }

        std::string_view name_of_message(const uint32_t message)
        {
            switch (message)
            {
            case WM_MOUSEHWHEEL:
                return "WM_MOUSEHWHEEL";
            case WM_XBUTTONDOWN:
                return "WM_XBUTTONDOWN";
            case WM_XBUTTONUP:
                return "WM_XBUTTONUP";
            case WM_LBUTTONDBLCLK:
                return "WM_LBUTTONDBLCLK";
            case WM_RBUTTONDBLCLK:
                return "WM_RBUTTONDBLCLK";
            case WM_MBUTTONDBLCLK:
                return "WM_MBUTTONDBLCLK";
            case WM_CHAR:
                return "WM_CHAR";
            case WM_SYSKEYDOWN:
                return "WM_SYSKEYDOWN";
            case WM_SYSKEYUP:
                return "WM_SYSKEYUP";
            default:
                return {};
            }
        }
    }

    std::vector<uint8_t> macos_encode_event_record(const macos_event_record& record)
    {
        tlv_writer out{};

        out.u32(FIELD_VERSION, 3);
        out.u32(FIELD_SUBVERSION, 0);
        out.u32(FIELD_TYPE, static_cast<uint32_t>(record.kind));
        out.point(FIELD_GLOBAL_POINT, record.global_x, record.global_y);
        out.point(FIELD_WINDOW_POINT, record.window_x, record.window_y);
        out.u64(FIELD_TIMESTAMP, record.timestamp);
        out.u32(FIELD_FLAGS, record.flags);
        out.u32(FIELD_WINDOW, record.window);
        out.u32(FIELD_CONNECTION, record.connection);
        out.u32(FIELD_POSTER_PID, record.poster_pid);
        out.u32(FIELD_COALESCED, 0);
        out.u32(FIELD_CONNECTION_AGAIN, record.connection);
        out.u32(FIELD_WINDOW_ORIGIN_Y, 0);
        out.u32(FIELD_WINDOW_HEIGHT, record.window_height);
        out.u32(FIELD_SCREEN_HEIGHT, record.screen_height);

        // No 0x10ae blob follows. Every host record carries one, holding a ~20-byte signature the real
        // WindowServer produces; sogen has no key to produce one with, and whether HIToolbox insists on
        // it is the open question this implementation exists to answer.

        if (is_mouse_button(record.kind))
        {
            out.u32(FIELD_BUTTON_DOWN, record.button_down ? 1 : 0);
            out.u32(FIELD_PRESSURE, record.button_down ? MOUSE_PRESSURE_DOWN : 0);
            out.u32(FIELD_BUTTON_NUMBER, record.button);
            out.u32(FIELD_EVENT_WINDOW_A, record.window);
            out.u32(FIELD_EVENT_WINDOW_B, record.window);
            out.u32(FIELD_CLICK_COUNT, record.click_count);
        }
        else if (record.kind == macos_event_kind::scroll_wheel)
        {
            out.u32(FIELD_SCROLL_LINES_1, static_cast<uint32_t>(record.scroll_axis1));
            out.u32(FIELD_SCROLL_LINES_2, static_cast<uint32_t>(record.scroll_axis2));
            out.u32(FIELD_SCROLL_LINES_3, 0);
            out.u32(FIELD_SCROLL_FIXED_1, static_cast<uint32_t>(record.scroll_axis1) << 16);
            out.u32(FIELD_SCROLL_FIXED_2, static_cast<uint32_t>(record.scroll_axis2) << 16);
            out.u32(FIELD_SCROLL_FIXED_3, 0);
            out.u32(FIELD_SCROLL_POINTS_1, static_cast<uint32_t>(record.scroll_axis1 * SCROLL_POINTS_PER_LINE));
            out.u32(FIELD_SCROLL_POINTS_2, static_cast<uint32_t>(record.scroll_axis2 * SCROLL_POINTS_PER_LINE));
            out.u32(FIELD_SCROLL_POINTS_3, 0);
        }
        else if (is_key(record.kind))
        {
            out.u32(FIELD_KEYCODE, record.keycode);
            out.u32(FIELD_KEY_ZERO, 0);
            out.u32(FIELD_KEYBOARD_ID, MACOS_EVENT_KEYBOARD_ID);
            out.u32(FIELD_KEY_CONSTANT, KEY_CONSTANT);
            out.u32(FIELD_CHARACTER, record.character);
            out.u32(FIELD_CHARACTER_UNMODIFIED, record.character);
            out.u32(FIELD_KEY_SUBTYPE, KEY_SUBTYPE);
            out.u32(FIELD_CHARACTER_AGAIN, record.character);
        }

        auto tlv = out.take();

        // Record version 3, big-endian, ahead of the first tag: the decoder reads it as the u32 at
        // record + 0 and refuses anything else.
        std::vector<uint8_t> payload{0x00, 0x00, 0x00, 0x03};
        payload.insert(payload.end(), tlv.begin(), tlv.end());
        return payload;
    }

    std::vector<uint8_t> macos_encode_datagram(const uint32_t type, const std::span<const uint8_t> payload)
    {
        std::vector<uint8_t> datagram(2 * sizeof(uint32_t) + payload.size(), 0);
        mach::write_u32(datagram, 0, type);
        mach::write_u32(datagram, 4, static_cast<uint32_t>(payload.size()));
        std::ranges::copy(payload, datagram.begin() + 2 * sizeof(uint32_t));
        return datagram;
    }

    macos_event_stream::~macos_event_stream()
    {
        this->uninstall();
    }

    void macos_event_stream::install(macos_emulator& emu)
    {
        if (this->owner_ == &emu)
        {
            return;
        }

        this->uninstall();
        this->owner_ = &emu;
        installed_streams()[&emu] = this;
    }

    void macos_event_stream::uninstall()
    {
        if (this->owner_ == nullptr)
        {
            return;
        }

        auto& streams = installed_streams();
        const auto found = streams.find(this->owner_);
        if (found != streams.end() && found->second == this)
        {
            streams.erase(found);
        }

        this->owner_ = nullptr;
    }

    void macos_event_stream::adopt_event_port(macos_emulator& emu, const uint32_t connection, const mach::port_name_t port)
    {
        if (connection == 0 || port == mach::PORT_NULL)
        {
            return;
        }

        const auto previous = this->ports_.find(connection);
        if (previous != this->ports_.end() && previous->second != port)
        {
            emu.log.info("connection 0x%x replaced its event port 0x%x with 0x%x\n", connection, previous->second, port);
        }

        this->ports_[connection] = port;
    }

    mach::port_name_t macos_event_stream::event_port_of(const uint32_t connection) const
    {
        const auto found = this->ports_.find(connection);
        return found == this->ports_.end() ? mach::PORT_NULL : found->second;
    }

    uint32_t macos_event_stream::connection_of_port(const mach::port_name_t port) const
    {
        for (const auto& [connection, name] : this->ports_)
        {
            if (name == port)
            {
                return connection;
            }
        }

        return 0;
    }

    bool macos_event_stream::post(macos_emulator& emu, const macos_event_record& record)
    {
        const auto port = this->event_port_of(record.connection);
        if (port == mach::PORT_NULL)
        {
            static std::set<uint32_t> reported{};
            if (reported.insert(record.connection).second)
            {
                emu.log.warn("connection 0x%x has no event port registered, so its input events are dropped\n", record.connection);
            }

            return false;
        }

        const auto datagram = macos_encode_datagram(MACOS_DATAGRAM_EVENT_RECORD, macos_encode_event_record(record));

        auto& stream = this->streams_[record.connection];
        if (stream.size() + datagram.size() > MACOS_EVENT_STREAM_MAX_BYTES)
        {
            emu.log.warn("the event stream for connection 0x%x is at its %zu byte limit; a type %u record is dropped\n", record.connection,
                         MACOS_EVENT_STREAM_MAX_BYTES, static_cast<uint32_t>(record.kind));
            return false;
        }

        const auto was_empty = stream.empty();
        stream.insert(stream.end(), datagram.begin(), datagram.end());

        if (!was_empty)
        {
            return true;
        }

        auto* entry = emu.mach.ports.destination_of(port);
        if (entry == nullptr)
        {
            emu.log.warn("the event port 0x%x of connection 0x%x names nothing in this namespace; no ping is sent\n", port,
                         record.connection);
            return false;
        }

        std::vector<uint8_t> ping(mach::MSG_HEADER_SIZE, 0);
        mach::write_msg_header(ping, mach::msg_header{
                                         .bits = PING_BITS,
                                         .size = mach::MSG_HEADER_SIZE,
                                         .remote_port = mach::PORT_NULL,
                                         .local_port = port,
                                         .voucher_port = mach::PORT_NULL,
                                         .id = 0,
                                     });

        entry->queue.push_back(std::move(ping));
        ++this->ping_count_;
        mach::announce_queued_message(emu, port, 0);
        return true;
    }

    std::vector<uint8_t> macos_event_stream::take_stream(const uint32_t connection)
    {
        const auto found = this->streams_.find(connection);
        if (found == this->streams_.end())
        {
            return {};
        }

        auto stream = std::move(found->second);
        this->streams_.erase(found);
        return stream;
    }

    size_t macos_event_stream::pending_bytes(const uint32_t connection) const
    {
        const auto found = this->streams_.find(connection);
        return found == this->streams_.end() ? 0 : found->second.size();
    }

    uint32_t macos_event_stream::connection_for_request(macos_emulator& emu, const mach::port_name_t port) const
    {
        // The connection port 32000 hands out carries the CID as its object id, so a pull that arrives on
        // it resolves exactly rather than falling through to the single-connection guess below.
        const auto object = emu.mach.ports.object_of(port);
        if (object.kind == mach::kernel_object_kind::window_server_event ||
            object.kind == mach::kernel_object_kind::window_server_connection)
        {
            return static_cast<uint32_t>(object.id);
        }

        // 30117 travels on the connection port, which the guest learns from the 32000 reply rather than
        // from SLSGetEventPort, so the port it arrives on need not be one this table knows. With a
        // single registered connection there is no ambiguity to resolve.
        if (this->ports_.size() == 1)
        {
            return this->ports_.begin()->first;
        }

        static std::set<mach::port_name_t> reported{};
        if (reported.insert(port).second)
        {
            emu.log.warn("a datagram pull arrived on port 0x%x, which names none of the %zu connections with an event port\n", port,
                         this->ports_.size());
        }

        return 0;
    }

    macos_event_stream* installed_event_stream(macos_emulator& emu)
    {
        auto& streams = installed_streams();
        const auto found = streams.find(&emu);
        return found == streams.end() ? nullptr : found->second;
    }

    std::vector<uint8_t> macos_event_stream_pull(macos_emulator& emu, const mach::mig_request& request)
    {
        std::vector<uint8_t> stream{};

        auto* events = installed_event_stream(emu);
        if (events == nullptr)
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("MIG %d (GetPortStreamOutofline) arrived with no event stream installed; the guest is told the stream is "
                             "empty\n",
                             MACOS_MIG_GET_PORT_STREAM_OUTOFLINE);
            }
        }
        else if (const auto connection = events->connection_for_request(emu, request.call.header.remote_port); connection != 0)
        {
            stream = events->take_stream(connection);
        }

        uint64_t address = 0;
        size_t allocated = 0;

        if (!stream.empty())
        {
            allocated = page_align_up(stream.size(), MACOS_PAGE_SIZE);
            address = emu.memory.allocate_memory(allocated, memory_permission::read_write, MACOS_DEFAULT_MMAP_BASE);

            if (address == 0 || !emu.memory.try_write_memory(address, stream.data(), stream.size()))
            {
                if (address != 0)
                {
                    emu.memory.release_memory(address, allocated);
                }

                emu.log.warn("no room for a %zu byte out-of-line event stream; the pull is answered as empty\n", stream.size());
                address = 0;
                stream.clear();
            }
        }

        const auto size = static_cast<uint32_t>(stream.size());

        std::vector<uint8_t> reply(
            mach::MSG_HEADER_SIZE + mach::MSG_BODY_SIZE + mach::OOL_DESCRIPTOR_SIZE + mach::NDR_RECORD_SIZE + sizeof(uint32_t), 0);

        mach::write_msg_header(reply, mach::msg_header{
                                          .bits = mach::reply_bits_for(request.call.header.bits, true),
                                          .size = static_cast<uint32_t>(reply.size()),
                                          .remote_port = mach::PORT_NULL,
                                          .local_port = request.call.header.local_port,
                                          .voucher_port = mach::PORT_NULL,
                                          .id = MACOS_MIG_GET_PORT_STREAM_OUTOFLINE_REPLY,
                                      });

        size_t offset = mach::MSG_HEADER_SIZE;
        mach::write_u32(reply, offset, 1);
        offset += mach::MSG_BODY_SIZE;

        // deallocate = 1 even on the empty answer, which the host does too: the client's teardown reads
        // the flag rather than the size.
        mach::write_ool_descriptor(std::span{reply}.subspan(offset), mach::ool_descriptor{
                                                                         .address = address,
                                                                         .size = size,
                                                                         .deallocate = 1,
                                                                         .copy = 1,
                                                                         .type = mach::descriptor_type::ool,
                                                                     });
        offset += mach::OOL_DESCRIPTOR_SIZE;

        std::ranges::copy(mach::NDR_RECORD, reply.begin() + static_cast<ptrdiff_t>(offset));
        offset += mach::NDR_RECORD_SIZE;

        mach::write_u32(reply, offset, size);
        return reply;
    }

    void register_event_stream_routines(mach::mig_server_table& table)
    {
        const auto pull = [](macos_emulator& emu, const mach::mig_request& request) { return macos_event_stream_pull(emu, request); };

        // The client sends 30117 to the connection port. sogen mints the window-server root port and,
        // where SLSGetEventPort ran, an event port of its own kind; the guest may reach either, so both
        // answer.
        table.register_routine(mach::kernel_object_kind::window_server, MACOS_MIG_GET_PORT_STREAM_OUTOFLINE, pull,
                               "GetPortStreamOutofline");
        table.register_routine(mach::kernel_object_kind::window_server_event, MACOS_MIG_GET_PORT_STREAM_OUTOFLINE, pull,
                               "GetPortStreamOutofline");
    }

    bool macos_translate_ui_event(macos_emulator& emu, const ui_event& event)
    {
        auto* events = installed_event_stream(emu);
        if (events == nullptr)
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("a ui_event arrived with no event stream installed; input is not delivered to the guest\n");
            }

            return false;
        }

        const auto* window = emu.ui.server.find_window(static_cast<uint32_t>(event.window));
        if (window == nullptr)
        {
            static std::set<uint64_t> reported{};
            if (reported.insert(event.window).second)
            {
                emu.log.warn("a ui_event names window %llu, which sogen's window server does not own\n",
                             static_cast<unsigned long long>(event.window));
            }

            return false;
        }

        macos_event_record record{};
        record.window = window->id;
        record.connection = window->connection;
        record.poster_pid = emu.process.pid;
        record.window_height = static_cast<uint32_t>(std::max(window->height, 0));
        record.screen_height = static_cast<uint32_t>(std::max(emu.ui.desktop_height, 0));
        record.timestamp = event_timestamp(emu);

        const auto client_x = static_cast<int16_t>(event.lParam & 0xFFFFu);
        const auto client_y = static_cast<int16_t>((event.lParam >> 16) & 0xFFFFu);
        record.window_x = static_cast<float>(client_x);
        record.window_y = static_cast<float>(client_y);
        record.global_x = static_cast<float>(window->x + client_x);
        record.global_y = static_cast<float>(window->y + client_y);

        const auto button_state = static_cast<uint32_t>(event.wParam & 0xFFFFu);
        const auto mouse_flags = static_cast<uint32_t>(((button_state & MK_SHIFT) != 0 ? MACOS_CG_FLAG_SHIFT : 0u) |
                                                       ((button_state & MK_CONTROL) != 0 ? MACOS_CG_FLAG_CONTROL : 0u));

        switch (event.message)
        {
        case WM_MOUSEMOVE:
            record.kind = macos_event_kind::mouse_moved;
            if ((button_state & MK_LBUTTON) != 0)
            {
                record.kind = macos_event_kind::left_mouse_dragged;
            }
            else if ((button_state & MK_RBUTTON) != 0)
            {
                record.kind = macos_event_kind::right_mouse_dragged;
            }
            else if ((button_state & MK_MBUTTON) != 0)
            {
                record.kind = macos_event_kind::other_mouse_dragged;
            }
            record.flags = mouse_flags;
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            const auto down = event.message == WM_LBUTTONDOWN || event.message == WM_RBUTTONDOWN || event.message == WM_MBUTTONDOWN;
            const auto right = event.message == WM_RBUTTONDOWN || event.message == WM_RBUTTONUP;
            const auto other = event.message == WM_MBUTTONDOWN || event.message == WM_MBUTTONUP;

            record.button = other ? 2u : (right ? 1u : 0u);
            record.button_down = down;
            record.click_count = down ? 1u : 0u;
            record.flags = mouse_flags;

            if (other)
            {
                record.kind = down ? macos_event_kind::other_mouse_down : macos_event_kind::other_mouse_up;
            }
            else if (right)
            {
                record.kind = down ? macos_event_kind::right_mouse_down : macos_event_kind::right_mouse_up;
            }
            else
            {
                record.kind = down ? macos_event_kind::left_mouse_down : macos_event_kind::left_mouse_up;
            }
            break;
        }

        case WM_MOUSEWHEEL:
            record.kind = macos_event_kind::scroll_wheel;
            record.flags = mouse_flags;
            record.scroll_axis1 = static_cast<int16_t>((event.wParam >> 16) & 0xFFFFu) / WHEEL_DELTA;
            break;

        case WM_KEYDOWN:
        case WM_KEYUP: {
            const auto keycode = macos_keycode_for_virtual_key(event.wParam);
            if (keycode == UINT32_MAX)
            {
                static std::set<uint64_t> reported{};
                if (reported.insert(event.wParam).second)
                {
                    emu.log.warn("virtual key 0x%llx has no macOS keycode in sogen's table; the key event is dropped\n",
                                 static_cast<unsigned long long>(event.wParam));
                }

                return false;
            }

            const auto down = event.message == WM_KEYDOWN;
            if (const auto bit = modifier_bit_for_virtual_key(event.wParam); bit != 0)
            {
                events->set_modifiers(down ? (events->modifiers() | bit) : (events->modifiers() & ~bit));
            }

            record.kind = down ? macos_event_kind::key_down : macos_event_kind::key_up;
            record.keycode = keycode;
            record.flags = events->modifiers();
            record.character = character_for_virtual_key(event.wParam, (events->modifiers() & MACOS_CG_FLAG_SHIFT) != 0);

            // Measured: a key record's window number is 0 and its window-relative point is the origin.
            record.window = 0;
            record.window_x = 0.0f;
            record.window_y = 0.0f;
            record.window_height = 0;
            break;
        }

        default: {
            static std::set<uint32_t> reported{};
            if (reported.insert(event.message).second)
            {
                const auto name = name_of_message(event.message);
                if (name.empty())
                {
                    emu.log.warn("ui_event message 0x%x has no measured macOS event record form and is dropped\n", event.message);
                }
                else
                {
                    emu.log.warn("%.*s has no measured macOS event record form and is dropped\n", static_cast<int>(name.size()),
                                 name.data());
                }
            }

            return false;
        }
        }

        return post_and_track(emu, *events, record);
    }

    void macos_update_scoreboard_cursor(macos_emulator& emu, const float x, const float y, const uint64_t timestamp)
    {
        const auto base = emu.ui.server.scoreboard_address;
        if (base == 0)
        {
            return;
        }

        const std::array<float, 2> cursor{x, y};
        const auto written = emu.memory.try_write_memory(base + MACOS_SCOREBOARD_CURSOR_OFFSET, cursor.data(), sizeof(cursor)) &&
                             emu.memory.try_write_memory(base + MACOS_SCOREBOARD_TIMESTAMP_OFFSET, &timestamp, sizeof(timestamp));

        if (!written)
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("the event scoreboard at 0x%llx is not writable, so the guest's cursor position never moves\n",
                             static_cast<unsigned long long>(base));
            }
        }
    }

    bool macos_post_input_event(macos_emulator& emu, const macos_input_event& input)
    {
        auto* events = installed_event_stream(emu);
        if (events == nullptr)
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("an input event arrived with no event stream installed; input is not delivered to the guest\n");
            }

            return false;
        }

        macos_event_record record{};
        record.kind = input.kind;
        record.global_x = static_cast<float>(input.screen_x);
        record.global_y = static_cast<float>(input.screen_y);
        record.timestamp = event_timestamp(emu);
        record.poster_pid = emu.process.pid;
        record.screen_height = static_cast<uint32_t>(std::max(emu.ui.desktop_height, 0));
        record.flags = input.flags;

        if (is_key(input.kind))
        {
            record.connection = emu.ui.server.key_connection();
            record.keycode = input.keycode;
            record.character = input.character;

            if (record.connection == 0)
            {
                static bool reported = false;
                if (!std::exchange(reported, true))
                {
                    emu.log.warn("a key event arrived while no window is on screen, so no connection owns it; it is dropped\n");
                }

                return false;
            }

            return post_and_track(emu, *events, record);
        }

        const auto* window = emu.ui.server.window_at(input.screen_x, input.screen_y);
        if (window == nullptr)
        {
            static bool reported = false;
            if (!std::exchange(reported, true))
            {
                emu.log.warn("an input event at (%d, %d) lands on no window sogen owns and is dropped\n", input.screen_x, input.screen_y);
            }

            return false;
        }

        record.window = window->id;
        record.connection = window->connection;
        record.window_x = static_cast<float>(input.screen_x - window->x);
        record.window_y = static_cast<float>(input.screen_y - window->y);
        record.window_height = static_cast<uint32_t>(std::max(window->height, 0));
        record.button = input.button;
        record.button_down = is_mouse_down(input.kind);
        record.click_count = record.button_down ? std::max(input.click_count, 1u) : 0u;
        record.scroll_axis1 = input.scroll_axis1;
        record.scroll_axis2 = input.scroll_axis2;

        return post_and_track(emu, *events, record);
    }
}
