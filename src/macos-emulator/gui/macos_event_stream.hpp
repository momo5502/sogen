#pragma once

#include "../std_include.hpp"

#include "../mach/mach_types.hpp"

#include <platform/ui_backend.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace sogen
{
    class macos_emulator;
}

namespace sogen::mach
{
    class mig_server_table;
    struct mig_request;
}

namespace sogen
{
    // CGSEventType. 1..12 coincide with CGEventType; 8 and 9 are the CGS-only entered/exited pair, and
    // 22 is the scroll wheel. Only the kinds sogen has a measured record form for are listed: an event
    // it cannot describe is reported by name rather than delivered as something else.
    enum class macos_event_kind : uint32_t
    {
        left_mouse_down = 1,
        left_mouse_up = 2,
        right_mouse_down = 3,
        right_mouse_up = 4,
        mouse_moved = 5,
        left_mouse_dragged = 6,
        right_mouse_dragged = 7,
        key_down = 10,
        key_up = 11,
        scroll_wheel = 22,
        other_mouse_down = 25,
        other_mouse_up = 26,
        other_mouse_dragged = 27,
    };

    // CGEventFlags, CGEventTypes.h. Measured on a shift+key-down: 0x20000.
    constexpr uint32_t MACOS_CG_FLAG_ALPHA_SHIFT = 0x00010000;
    constexpr uint32_t MACOS_CG_FLAG_SHIFT = 0x00020000;
    constexpr uint32_t MACOS_CG_FLAG_CONTROL = 0x00040000;
    constexpr uint32_t MACOS_CG_FLAG_ALTERNATE = 0x00080000;
    constexpr uint32_t MACOS_CG_FLAG_COMMAND = 0x00100000;

    // Measured 25G76: the keyboard id every key record carries, and the value SLSGetLastUsedKeyboardID
    // hands back on the same host.
    constexpr uint32_t MACOS_EVENT_KEYBOARD_ID = 0x5c;

    // The datagram types CGSDatagramReadStream splits a pulled stream into.
    constexpr uint32_t MACOS_DATAGRAM_EVENT_RECORD = 0;
    constexpr uint32_t MACOS_DATAGRAM_NOTIFY = 1;

    // MIG on the window-server connection port: the client pulls the stream with 30117 and the server
    // answers 30217 with one out-of-line descriptor.
    constexpr int32_t MACOS_MIG_GET_PORT_STREAM_OUTOFLINE = 30117;
    constexpr int32_t MACOS_MIG_GET_PORT_STREAM_OUTOFLINE_REPLY = 30217;

    // A stream longer than this is not delivered. The real server has no such limit; sogen needs one
    // because the buffer is a guest allocation the emulator makes on the guest's behalf.
    constexpr size_t MACOS_EVENT_STREAM_MAX_BYTES = 0x100000;

    // One event, in the terms the wire record is written in. Coordinates are CG screen space: y down
    // from the top of the primary display, which is the space macos_window already uses.
    struct macos_event_record
    {
        macos_event_kind kind{};
        float global_x{};
        float global_y{};
        float window_x{};
        float window_y{};
        uint64_t timestamp{};
        uint32_t flags{};
        uint32_t window{};
        uint32_t connection{};
        uint32_t poster_pid{};
        uint32_t window_height{};
        uint32_t screen_height{};

        // Mouse: 0 left, 1 right, 2 other -- measured field 0x14003.
        uint32_t button{};
        bool button_down{};
        uint32_t click_count{};

        // Scroll: lines on axis 1 (vertical) and 2 (horizontal). The fixed-point and point-valued
        // copies the record also carries are derived from these.
        int32_t scroll_axis1{};
        int32_t scroll_axis2{};

        uint32_t keycode{};
        uint32_t character{};
    };

    // One input event as a host front-end has it: a point on the emulated desktop in CG screen
    // coordinates, and nothing about windows. Which window the point lands on, which connection owns
    // that window, and where the point falls inside it are sogen's to work out.
    struct macos_input_event
    {
        macos_event_kind kind{};
        int32_t screen_x{};
        int32_t screen_y{};
        uint32_t button{};
        uint32_t click_count{1};
        uint32_t keycode{};
        uint32_t character{};
        uint32_t flags{};
        int32_t scroll_axis1{};
        int32_t scroll_axis2{};
    };

    // The cursor position SLSCurrentInputPointerPosition hands back: two little-endian f32 at the start
    // of the first event-shmem page, widened with fcvtl. Measured on 25G76 -- see
    constexpr uint64_t MACOS_SCOREBOARD_CURSOR_OFFSET = 0x00;
    constexpr uint64_t MACOS_SCOREBOARD_TIMESTAMP_OFFSET = 0x18;

    // Field ids as they appear in a tag's low half. A tag is [u16 field][u16 count]; the payload is
    // count bytes when the field has 0x1000, count u32s when it has 0x4000, and count u64s otherwise.
    std::vector<uint8_t> macos_encode_event_record(const macos_event_record& record);
    std::vector<uint8_t> macos_encode_datagram(uint32_t type, std::span<const uint8_t> payload);

    // Owns the per-connection event ports and the byte streams behind them. Registers itself with the
    // MIG routines and the ui_event translator while it lives, so nothing outlives its owner.
    class macos_event_stream
    {
      public:
        macos_event_stream() = default;
        ~macos_event_stream();

        macos_event_stream(const macos_event_stream&) = delete;
        macos_event_stream& operator=(const macos_event_stream&) = delete;
        macos_event_stream(macos_event_stream&&) = delete;
        macos_event_stream& operator=(macos_event_stream&&) = delete;

        void install(macos_emulator& emu);
        void uninstall();

        // The client constructs its own event port inside SLSNewConnection and ships it in the 32000
        // request, so sogen records the association rather than minting the port.
        void adopt_event_port(macos_emulator& emu, uint32_t connection, mach::port_name_t port);
        mach::port_name_t event_port_of(uint32_t connection) const;
        uint32_t connection_of_port(mach::port_name_t port) const;

        // Appends one type-0 datagram, and on the empty -> non-empty edge pings the connection's event
        // port with the 24-byte header-only message the window server sends.
        bool post(macos_emulator& emu, const macos_event_record& record);

        std::vector<uint8_t> take_stream(uint32_t connection);
        size_t pending_bytes(uint32_t connection) const;

        // Which connection a 30117 arriving on `port` is asking about. Zero when that cannot be decided,
        // which is reported by name once.
        uint32_t connection_for_request(macos_emulator& emu, mach::port_name_t port) const;

        size_t ping_count() const
        {
            return this->ping_count_;
        }

        // ui_event carries no modifier state on a key message -- only the virtual key -- so the
        // translator keeps its own, the way a keyboard driver does.
        uint32_t modifiers() const
        {
            return this->modifiers_;
        }

        void set_modifiers(uint32_t flags)
        {
            this->modifiers_ = flags;
        }

      private:
        std::map<uint32_t, mach::port_name_t> ports_{};
        std::map<uint32_t, std::vector<uint8_t>> streams_{};
        macos_emulator* owner_{};
        size_t ping_count_{};
        uint32_t modifiers_{};
    };

    macos_event_stream* installed_event_stream(macos_emulator& emu);

    std::vector<uint8_t> macos_event_stream_pull(macos_emulator& emu, const mach::mig_request& request);
    void register_event_stream_routines(mach::mig_server_table& table);

    // Turns the emulator's generic ui_event into a record and posts it. False when the event names a
    // window sogen does not own, or a message with no measured record form -- both reported by name.
    bool macos_translate_ui_event(macos_emulator& emu, const ui_event& event);

    // The same delivery from a desktop point rather than from a window handle: the entry point a host
    // front-end with no window of its own -- a headless run, a browser canvas -- injects through. False
    // when nothing visible is under the point, or no connection owns a keystroke yet.
    bool macos_post_input_event(macos_emulator& emu, const macos_input_event& input);

    // Keeps the event scoreboard's cursor position in step with the pointer. A guest reads it through
    // SLSGetCurrentCursorLocation rather than out of an event record, so a stale one is a cursor that
    // never moves.
    void macos_update_scoreboard_cursor(macos_emulator& emu, float x, float y, uint64_t timestamp);
}
