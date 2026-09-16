#include "../std_include.hpp"
#include "macos_window_server_mig.hpp"

#include "macos_display_state.hpp"
#include "macos_event_stream.hpp"
#include "macos_ui_state.hpp"

#include "../mach/mig_kernel_servers.hpp"
#include "../macos_emulator.hpp"
#include "skylight_routines.hpp"

#include <address_utils.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

namespace sogen
{
    namespace
    {
        using mach::mig_reply_builder;
        using mach::mig_request;

        std::vector<uint8_t> fail(const mig_request& request, const mach::kern_return_t code)
        {
            return mach::make_mig_error_bytes(request, code);
        }

        // One page of guest memory plus the memory entry that names it. The guest maps the entry with
        // mach_vm_map and gets this same range back -- sogen has one address space, so a shared page is
        // shared by being the same page rather than by being mapped twice.
        struct shared_page
        {
            uint64_t address{};
            mach::port_name_t entry{};
        };

        std::optional<shared_page> make_shared_page(macos_emulator& emu, const uint64_t least_bytes)
        {
            const auto length = static_cast<size_t>(page_align_up(std::max<uint64_t>(least_bytes, 1), MACOS_PAGE_SIZE));
            const auto address = emu.memory.allocate_memory(length, memory_permission::read_write, MACOS_DEFAULT_MMAP_BASE);
            if (address == 0)
            {
                emu.log.warn("no room for a %zu byte window-server shared page\n", length);
                return std::nullopt;
            }

            const std::vector<uint8_t> zeroes(length, 0);
            if (!emu.memory.try_write_memory(address, zeroes.data(), zeroes.size()))
            {
                emu.memory.release_memory(address, length);
                emu.log.warn("a window-server shared page at 0x%llx could not be cleared\n", static_cast<unsigned long long>(address));
                return std::nullopt;
            }

            const auto entry = emu.mach.create_memory_entry(address, length);
            if (entry == mach::PORT_NULL)
            {
                emu.memory.release_memory(address, length);
                return std::nullopt;
            }

            return shared_page{.address = address, .entry = entry};
        }

        void append_port(mig_reply_builder& builder, const mach::port_name_t name)
        {
            builder.append_port_descriptor(
                {.name = name, .disposition = mach::disposition::make_send, .type = mach::descriptor_type::port});
        }

        // The reply shape 34003 and 30456 share with 30117: one out-of-line descriptor, the NDR record,
        // and the byte count repeated after it. An empty answer keeps the descriptor and sets the address
        // and size to zero, which is what the host sends for "nothing changed".
        std::vector<uint8_t> make_ool_reply(macos_emulator& emu, const mig_request& request, const std::span<const uint8_t> payload)
        {
            uint64_t address = 0;
            uint32_t size = 0;

            if (!payload.empty())
            {
                const auto length = static_cast<size_t>(page_align_up(payload.size(), MACOS_PAGE_SIZE));
                address = emu.memory.allocate_memory(length, memory_permission::read_write, MACOS_DEFAULT_MMAP_BASE);

                if (address == 0 || !emu.memory.try_write_memory(address, payload.data(), payload.size()))
                {
                    if (address != 0)
                    {
                        emu.memory.release_memory(address, length);
                        address = 0;
                    }

                    emu.log.warn("no room for a %zu byte out-of-line reply to MIG %d; it is answered as empty\n", payload.size(),
                                 request.call.header.id);
                }
                else
                {
                    size = static_cast<uint32_t>(payload.size());
                }
            }

            std::vector<uint8_t> reply(
                mach::MSG_HEADER_SIZE + mach::MSG_BODY_SIZE + mach::OOL_DESCRIPTOR_SIZE + mach::NDR_RECORD_SIZE + sizeof(uint32_t), 0);

            mach::write_msg_header(reply, mach::msg_header{
                                              .bits = mach::reply_bits_for(request.call.header.bits, true),
                                              .size = static_cast<uint32_t>(reply.size()),
                                              .remote_port = mach::PORT_NULL,
                                              .local_port = request.call.header.local_port,
                                              .voucher_port = mach::PORT_NULL,
                                              .id = request.call.header.id + mach::subsystem::reply_offset,
                                          });

            size_t offset = mach::MSG_HEADER_SIZE;
            mach::write_u32(reply, offset, 1);
            offset += mach::MSG_BODY_SIZE;

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

        std::vector<uint8_t> simple_status_reply(macos_emulator& emu, const mig_request& request, const uint32_t status)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(status);
            return builder.finish();
        }

        mach::port_name_t session_port(macos_emulator& emu)
        {
            return macos_window_server_session_port(emu);
        }

        // 29010. The root port a bootstrap lookup of com.apple.windowserver.active would produce answers
        // with the session port; sogen's SLSServerPort hands out that same port directly, so this only
        // fires for a guest that reached the root port some other way.
        std::vector<uint8_t> get_session_port(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, session_port(emu));
            return builder.finish();
        }

        // 29000. Measured reply: the session port again, the CoreGraphics server version, and two words
        // whose meaning SkyLight never reads back on this build.
        std::vector<uint8_t> get_core_graphics_server_version(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, session_port(emu));
            builder.append_ndr();
            builder.append_u32(MACOS_CG_SERVER_VERSION);
            builder.append_u32(0);
            builder.append_u32(1);
            return builder.finish();
        }

        // 29004. A port the client parks a death notification on. Nothing in sogen ever dies, so it is a
        // live receive right that stays silent -- which is what the client does with the real one too.
        std::vector<uint8_t> session_death_watch_port(macos_emulator& emu, const mig_request& request)
        {
            auto& server = emu.ui.server;
            if (server.session_death_watch_port == mach::PORT_NULL)
            {
                server.session_death_watch_port =
                    emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::window_server, .id = 2});
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, server.session_death_watch_port);
            return builder.finish();
        }

        // 32003. Measured: status 0 and an all-zero option word. Every debug option off is the state a
        // WindowServer without CG_DEBUG in its environment reports.
        std::vector<uint8_t> get_debug_options(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(mach::kr::success));
            builder.append_u32(0);
            return builder.finish();
        }

        // 32006. Two memory entries, mapped by CGSEventScoreboardCreateFromShmemEntries. The scoreboard
        // is where the cursor position is read from; no event record ever travels through it.
        std::vector<uint8_t> get_event_shmem(macos_emulator& emu, const mig_request& request)
        {
            auto& server = emu.ui.server;
            if (server.event_shmem_entries.empty())
            {
                const auto scoreboard = make_shared_page(emu, MACOS_EVENT_SCOREBOARD_BYTES);
                const auto companion = make_shared_page(emu, MACOS_EVENT_SCOREBOARD_AUX_BYTES);
                if (!scoreboard || !companion)
                {
                    return fail(request, mach::kr::resource_shortage);
                }

                server.scoreboard_address = scoreboard->address;
                server.event_shmem_entries = {scoreboard->entry, companion->entry};
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            for (const auto entry : server.event_shmem_entries)
            {
                append_port(builder, entry);
            }

            return builder.finish();
        }

        // 34006. Three memory entries the display machinery maps before it asks for the display system
        // state. Their layout is not measured; they are pages of zeroes, which is what a session with no
        // attached display would leave behind.
        std::vector<uint8_t> get_display_shmem(macos_emulator& emu, const mig_request& request)
        {
            auto& server = emu.ui.server;
            if (server.display_shmem_entries.empty())
            {
                const auto header = make_shared_page(emu, MACOS_DISPLAY_SHMEM_HEADER_BYTES);
                const auto state = make_shared_page(emu, MACOS_DISPLAY_SHMEM_STATE_BYTES);
                const auto modes = make_shared_page(emu, MACOS_DISPLAY_SHMEM_MODES_BYTES);
                if (!header || !state || !modes)
                {
                    return fail(request, mach::kr::resource_shortage);
                }

                server.display_shmem_entries = {header->entry, state->entry, modes->entry};
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            for (const auto entry : server.display_shmem_entries)
            {
                append_port(builder, entry);
            }

            return builder.finish();
        }

        // 34003. The request carries the generation the client already knows; the reply is an
        // out-of-line blob of everything newer, and empty when there is nothing newer. That blob is the
        // whole CGSDisplaySystemState SkyLight then answers every display, mode and UUID query out of --
        // measured, and the reason NSScreen was empty while the display exports were being intercepted
        // one by one. sogen's configuration never changes, so it is serialized once and every later
        // request, which carries that same generation back, is answered as empty.
        std::vector<uint8_t> get_display_system_state(macos_emulator& emu, const mig_request& request)
        {
            if (request.arg_u64(0) == MACOS_DISPLAY_STATE_GENERATION)
            {
                return make_ool_reply(emu, request, {});
            }

            const macos_display_description display{
                .id = MACOS_MAIN_DISPLAY_ID,
                .width = static_cast<double>(emu.ui.desktop_width),
                .height = static_cast<double>(emu.ui.desktop_height),
            };

            const auto state = macos_build_display_system_state(std::span{&display, 1}, MACOS_DISPLAY_STATE_GENERATION);
            return make_ool_reply(emu, request, state);
        }

        // 30456. The same out-of-line shape, carrying the HID device list. sogen has no HID devices to
        // enumerate; the guest's own keyboard and mouse handling does not depend on one.
        std::vector<uint8_t> get_devices(macos_emulator& emu, const mig_request& request)
        {
            return make_ool_reply(emu, request, {});
        }

        // 30553. Measured reply on a host with the default layout: status 0 and a zero-length mapping.
        std::vector<uint8_t> get_unified_key_mapping(macos_emulator& emu, const mig_request& request)
        {
            mig_reply_builder builder{request.call, emu.mach.ports};
            builder.append_ndr();
            builder.append_u32(static_cast<uint32_t>(mach::kr::success));
            builder.append_u32(0);
            return builder.finish();
        }

        // The 32000 request ends with a variable-length string, so its trailing words are not at a fixed
        // argument index; the CAContext id is simply the last one.
        uint32_t trailing_u32(const mig_request& request)
        {
            const auto args = request.effective_args_offset();
            if (request.body.size() < args + sizeof(uint32_t))
            {
                return 0;
            }

            return mach::read_u32(request.body, request.body.size() - sizeof(uint32_t));
        }

        uint32_t connection_of(macos_emulator& emu, const mig_request& request)
        {
            const auto* connection = emu.ui.server.connection_for_port(request.call.header.remote_port);
            return connection == nullptr ? 0 : connection->id;
        }

        // 30378 and 30379. The client declares which notification kinds it wants; sogen posts none of
        // them, so the registration is recorded as accepted and the list is reported once so a guest
        // waiting on a notification names it rather than hanging silently.
        std::vector<uint8_t> set_notify_interests(macos_emulator& emu, const mig_request& request)
        {
            const auto count = request.arg_u32(1);
            static std::set<std::pair<int32_t, uint32_t>> reported{};

            for (uint32_t i = 0; i < count && i < 64; ++i)
            {
                const auto notification = request.arg_u32(2 + i);
                if (reported.emplace(request.call.header.id, notification).second)
                {
                    emu.log.info("window-server notification %u is registered for by connection 0x%x and never posted\n", notification,
                                 connection_of(emu, request));
                }
            }

            return simple_status_reply(emu, request, static_cast<uint32_t>(mach::kr::success));
        }

        // 32000. The one routine on the critical path for input: the request carries the event port the
        // client built with mach_port_construct, and the association recorded here is what lets a ui
        // event reach the guest at all. SLSNewConnection type-checks the reply hard -- complex, msgh_size
        // exactly 64, two port descriptors both received as move-send -- so nothing may be added to it.
        std::vector<uint8_t> new_connection_port(macos_emulator& emu, const mig_request& request)
        {
            auto& ui = emu.ui;

            const auto event_port = request.descriptor(0);
            if (!event_port.has_value() || event_port->type != mach::descriptor_type::port)
            {
                emu.log.warn("MIG %d (NewConnectionPort) arrived without the client's event port\n", MACOS_MIG_NEW_CONNECTION_PORT);
                return fail(request, mach::kr::invalid_argument);
            }

            const auto page = make_shared_page(emu, MACOS_CONNECTION_SHMEM_BYTES);
            if (!page)
            {
                return fail(request, mach::kr::resource_shortage);
            }

            const auto id = ui.server.create_connection();
            auto* connection = ui.server.find_connection(id);

            connection->port =
                emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::window_server_connection, .id = id});
            connection->shmem_entry = page->entry;
            connection->event_port = event_port->name;

            // The last word of the request is the CAContext id the client got from the render server.
            // Recorded rather than used: sogen composites the layer tree itself.
            connection->ca_context = trailing_u32(request);

            ui.events.adopt_event_port(emu, id, connection->event_port);
            emu.log.info("window-server connection 0x%x is open on port 0x%x with event port 0x%x\n", id, connection->port,
                         connection->event_port);

            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, connection->port);
            append_port(builder, connection->shmem_entry);
            builder.append_ndr();
            builder.append_u32(id);
            return builder.finish();
        }

        // 40202. CA::Context::connect_remote runs inside SLSNewConnection, so the render server is part
        // of the connection bring-up whether or not anything is rendered through it. Reply descriptor 0
        // is the context's own port -- where a later fence transaction goes -- and descriptor 1 is the
        // port a CAHostingToken is minted against. The empty server UUID keeps
        // CA::Render::Context::validate_server_uuid a no-op, which it already is off an internal build.
        std::vector<uint8_t> ca_register_client(macos_emulator& emu, const mig_request& request)
        {
            auto& server = emu.ui.server;

            const auto client = server.next_render_client++;
            const auto context = emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::render_server, .id = client});
            const auto hosting = emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::render_server, .id = client});

            if (context == mach::PORT_NULL || hosting == mach::PORT_NULL)
            {
                return fail(request, mach::kr::resource_shortage);
            }

            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, context);
            append_port(builder, hosting);
            builder.append_ndr();
            builder.append_u32(client);
            builder.append_u32(MACOS_CA_SESSION_ID);
            builder.append_u32(0);
            builder.append_u32(0);
            builder.append_u32(0);

            auto reply = builder.finish();
            if (reply.size() != MACOS_CA_REGISTER_CLIENT_REPLY_BYTES)
            {
                emu.log.warn("the RegisterClient reply is %zu bytes; _CASRegisterClient accepts only %zu\n", reply.size(),
                             MACOS_CA_REGISTER_CLIENT_REPLY_BYTES);
            }

            return reply;
        }

        // 30082. CGSWindowGetMappedImpl asks for the window's descriptor page whenever the client-side
        // window record is missing, which under sogen is always: SLSNewWindowWithOpaqueShape is
        // intercepted, so SkyLight's own CGSWindowConstructWithRegionImpl never ran and never registered
        // the window locally. The reply is a memory entry plus the owning connection id, a layout version
        // and a reserved word; on failure the measured RetCode is kCGErrorFailure, which makes the client
        // build a window with no descriptor -- and AppKit panics on that, so this has to succeed.
        std::vector<uint8_t> get_window_shmem_reference(macos_emulator& emu, const mig_request& request)
        {
            auto* window = emu.ui.server.find_window(request.arg_u32(0));
            if (window == nullptr)
            {
                return fail(request, static_cast<mach::kern_return_t>(MACOS_CG_ERROR_FAILURE));
            }

            if (window->shmem_entry == mach::PORT_NULL)
            {
                const auto page = make_shared_page(emu, MACOS_WINDOW_SHMEM_BYTES);
                if (!page)
                {
                    return fail(request, mach::kr::resource_shortage);
                }

                window->shmem_address = page->address;
                window->shmem_entry = page->entry;
            }

            macos_window_shmem_refresh(emu, *window);

            mig_reply_builder builder{request.call, emu.mach.ports};
            append_port(builder, window->shmem_entry);
            builder.append_ndr();
            builder.append_u32(window->connection);
            builder.append_u32(MACOS_WINDOW_SHMEM_LAYOUT_VERSION);
            builder.append_u32(0);
            return builder.finish();
        }

        std::vector<uint8_t> pull_port_stream(macos_emulator& emu, const mig_request& request)
        {
            return macos_event_stream_pull(emu, request);
        }
    }

    mach::port_name_t macos_window_server_session_port(macos_emulator& emu)
    {
        auto& server = emu.ui.server;
        if (server.server_port == mach::PORT_NULL)
        {
            const auto receive = emu.mach.ports.allocate_receive_right({.kind = mach::kernel_object_kind::window_server, .id = 1});
            server.server_port = emu.mach.ports.insert_send_right(receive);
        }

        return server.server_port;
    }

    // The descriptor page as 25G76's WindowServer fills it, measured on two live windows: three seed
    // counters, the window's screen origin, the window-to-screen affine and its inverse translation,
    // a unit resolution and the owning connection id repeated three times. Every offset not written
    // here read back zero on the host too.
    void macos_window_shmem_refresh(macos_emulator& emu, const macos_window& window)
    {
        if (window.shmem_address == 0)
        {
            return;
        }

        std::array<uint8_t, MACOS_WINDOW_SHMEM_BYTES> page{};

        const auto put_u32 = [&page](const size_t offset, const uint32_t value) {
            std::memcpy(page.data() + offset, &value, sizeof(value));
        };

        const auto put_f32 = [&page](const size_t offset, const float value) { std::memcpy(page.data() + offset, &value, sizeof(value)); };

        const auto origin_x = static_cast<float>(window.x);
        const auto origin_y = static_cast<float>(window.y);

        put_u32(0x00, 1);
        put_u32(0x04, 1);
        put_u32(0x08, 1);
        put_f32(0x0c, origin_x);
        put_f32(0x10, origin_y);
        put_f32(0x14, 1.0f);
        put_f32(0x20, 1.0f);
        put_f32(0x24, -origin_x);
        put_f32(0x28, -origin_y);
        put_f32(0x2c, 1.0f);
        put_f32(0x40, 1.0f);
        put_f32(0x54, 1.0f);
        put_f32(0x5c, -origin_x);
        put_f32(0x60, -origin_y);
        put_f32(0x68, 1.0f);
        put_u32(0x6c, 0xffffffffu);
        put_u32(0x98, window.connection);
        put_u32(0x9c, window.connection);
        put_u32(0xa0, window.connection);

        emu.memory.try_write_memory(window.shmem_address, page.data(), page.size());
    }

    void register_window_server_mig_routines(mach::mig_server_table& table)
    {
        constexpr auto root = mach::kernel_object_kind::window_server;
        constexpr auto connection = mach::kernel_object_kind::window_server_connection;

        table.register_routine(root, MACOS_MIG_GET_CORE_GRAPHICS_SERVER_VERSION, get_core_graphics_server_version,
                               "GetCoreGraphicsServerVersion");
        table.register_routine(root, MACOS_MIG_SESSION_DEATH_WATCH_PORT, session_death_watch_port, "SessionDeathWatchPort");
        table.register_routine(root, MACOS_MIG_GET_SESSION_PORT, get_session_port, "GetSessionPort");
        table.register_routine(root, MACOS_MIG_NEW_CONNECTION_PORT, new_connection_port, "NewConnectionPort");
        table.register_routine(root, MACOS_MIG_GET_DEBUG_OPTIONS, get_debug_options, "GetDebugOptions");
        table.register_routine(root, MACOS_MIG_GET_EVENT_SHMEM, get_event_shmem, "GetEventShmem");
        table.register_routine(root, MACOS_MIG_GET_DISPLAY_SYSTEM_STATE, get_display_system_state, "GetDisplaySystemState");
        table.register_routine(root, MACOS_MIG_GET_DISPLAY_SHMEM, get_display_shmem, "GetDisplayShmem");

        table.register_routine(connection, MACOS_MIG_SET_PROCESS_NOTIFY_INTERESTS, set_notify_interests, "SetProcessNotifyInterests");
        table.register_routine(connection, MACOS_MIG_SET_CONNECTION_NOTIFY_INTERESTS, set_notify_interests, "SetConnectionNotifyInterests");
        table.register_routine(connection, MACOS_MIG_GET_WINDOW_SHMEM_REFERENCE, get_window_shmem_reference, "GetWindowShmemReference");
        table.register_routine(connection, MACOS_MIG_GET_DEVICES, get_devices, "GetDevices");
        table.register_routine(connection, MACOS_MIG_GET_UNIFIED_KEY_MAPPING, get_unified_key_mapping, "GetUnifiedKeyMapping");

        // The datagram pull travels on the connection port, which is the port 32000 handed out rather
        // than one the event stream minted, so the stream's own registration does not cover it.
        table.register_routine(connection, MACOS_MIG_GET_PORT_STREAM_OUTOFLINE, pull_port_stream, "GetPortStreamOutofline");

        table.register_routine(mach::kernel_object_kind::render_server, MACOS_MIG_CA_REGISTER_CLIENT, ca_register_client, "RegisterClient");
    }
}
