#pragma once

#include "../std_include.hpp"

#include <platform/ui_backend.hpp>
#include <serialization.hpp>

#include <vector>

namespace sogen
{
    // The id the first connection gets. Any non-zero value would do; a real one is a per-boot handle
    // from the daemon, and nothing in a guest may assume a particular number.
    constexpr uint32_t MACOS_MAIN_CONNECTION_ID = 0x1D01;

    // One CGSConnection as the server side sees it. SkyLight's client builds its own object out of the
    // 32000 NewConnectionPort reply; these are the halves of that exchange sogen has to remember, so a
    // later message on the connection port names a connection rather than being guessed at.
    struct macos_connection
    {
        uint32_t id{};

        // The port sogen answers the 30xxx CGXServices traffic on, handed over as reply descriptor 0.
        uint32_t port{};

        // The port the client constructed with mach_port_construct before it sent 32000, and the one the
        // datagram-available ping goes to. sogen never mints it.
        uint32_t event_port{};

        // Reply descriptor 1: a memory entry the client maps at conn+0x60. Its contents are never read
        // back by anything sogen has measured.
        uint32_t shmem_entry{};

        // The CAContext id the client put in the 32000 request, kept so a render-server routine can be
        // attributed to a connection.
        uint32_t ca_context{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct macos_window
    {
        uint32_t id{};
        uint32_t connection{};
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};
        int32_t level{};
        bool ordered_in{};

        // An opaque window's alpha channel is not consulted by the compositor, which is what the word
        // means: a freshly created backing store is all zeroes, and blending that would make an opaque
        // window invisible rather than black.
        bool opaque{};
        std::u16string title{};
        uint64_t backing_address{};
        uint32_t backing_stride{};
        uint64_t context{};
        uint64_t layer_context{};

        // The per-window descriptor MIG 30082 hands out as a memory entry. SkyLight maps it read-only
        // and keeps reading it, so sogen owns the page for the window's lifetime and refreshes it
        // whenever the geometry changes.
        uint64_t shmem_address{};
        uint32_t shmem_entry{};

        // SLSSetEventMask / SLSSetWindowClientPerceivedType: recorded, read by nobody yet. The input
        // stage (M4) is the consumer; registering the association honestly is what this stage owes it.
        uint64_t event_mask{};
        uint32_t perceived_type{};

        size_t backing_bytes() const;
        RECT rect() const;

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct macos_region
    {
        uint32_t id{};
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    // One pending SLSTransactionSetWindowShape. Resolved when the routine runs -- the host calls
    // CGSGetRegionData on the region argument immediately, so the rect is fixed at set time, not at
    // commit time.
    struct macos_pending_shape
    {
        uint32_t window_id{};
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    // SLSTransactionRef on the host is a CFRuntime object that survives being committed: the same ref
    // was measured committed 15+ times in one appkitwin run, so commit clears the pending ops but keeps
    // the transaction alive. Its lifetime ends guest-side via CFRelease, which sogen never sees.
    struct macos_transaction
    {
        uint64_t id{};
        uint32_t connection{};
        std::vector<macos_pending_shape> shapes{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    // What SkyLight would otherwise keep inside the WindowServer daemon. Every id a guest can hand back
    // is looked up here rather than dereferenced, so a wrong or hostile one is a failed lookup.
    class macos_window_server
    {
      public:
        uint32_t main_connection() const
        {
            return MACOS_MAIN_CONNECTION_ID;
        }

        uint32_t create_connection();
        bool has_connection(uint32_t connection) const;
        macos_connection* find_connection(uint32_t connection);
        const macos_connection* find_connection(uint32_t connection) const;
        const macos_connection* connection_for_port(uint32_t port) const;

        const std::vector<macos_connection>& connections() const
        {
            return this->connections_;
        }

        macos_window* create_window(uint32_t connection, int32_t x, int32_t y, int32_t width, int32_t height);
        macos_window* find_window(uint32_t id);
        const macos_window* find_window(uint32_t id) const;
        bool destroy_window(uint32_t id);

        // Which window a point on the emulated desktop belongs to, in the CG screen space macos_window
        // already uses. Creation order is the z-order every consumer of this server composes in, so the
        // search runs backwards: the window painted last is the one in front.
        const macos_window* window_at(int32_t x, int32_t y) const;

        // A key record carries window number 0, so its target cannot come from a hit test. The frontmost
        // visible window's owner is the connection a keystroke belongs to; zero when nothing is visible.
        uint32_t key_connection() const;

        uint32_t create_region(int32_t x, int32_t y, int32_t width, int32_t height);
        const macos_region* find_region(uint32_t id) const;

        uint64_t create_transaction(uint32_t connection);

        // A transaction SkyLight's own SLSTransactionCreate built. sogen never sees that call return,
        // so the record is made the first time the handle reaches an intercepted routine.
        macos_transaction& adopt_transaction(uint64_t id, uint32_t connection);
        macos_transaction* find_transaction(uint64_t id);
        const macos_transaction* find_transaction(uint64_t id) const;
        // Applies the pending shapes to the windows and clears them. False on an unknown transaction.
        bool commit_transaction(uint64_t id);

        // Port names the GUI handlers minted in the mach namespace. The server only remembers them; the
        // namespace owns the ports. Zero until the matching routine ran.
        uint32_t server_port{};
        uint32_t render_server_port{};
        uint32_t event_port{};
        uint32_t session_death_watch_port{};

        // Memory entries over pages sogen owns: 32006 GetEventShmem hands back two (the event
        // scoreboard and its companion), 34006 GetDisplayShmem three. Empty until the routine ran.
        std::vector<uint32_t> event_shmem_entries{};
        std::vector<uint32_t> display_shmem_entries{};

        // The guest address of the first event-shmem page. The scoreboard's cursor position lives at its
        // start, which is where a guest reads the pointer from rather than from an event record.
        uint64_t scoreboard_address{};

        // The CAContext ids the render server hands out. QuartzCore keeps one per remote context and puts
        // it in the 32000 request, so a zero would read as "no context".
        uint32_t next_render_client{1};

        // The SLPS registration handshake (30283 CreateApplication rides SLPSRegisterWithServer).
        bool process_registered{};
        uint32_t main_application_connection{};
        bool front_process_set{};

        const std::vector<macos_window>& windows() const
        {
            return this->windows_;
        }

        std::vector<macos_window>& windows()
        {
            return this->windows_;
        }

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::vector<macos_window> windows_{};
        std::vector<macos_region> regions_{};
        std::vector<macos_connection> connections_{};
        std::vector<macos_transaction> transactions_{};

        uint32_t next_window_{1};
        uint32_t next_region_{1};
        uint32_t next_connection_{MACOS_MAIN_CONNECTION_ID};
        uint64_t next_transaction_{1};
    };
}
