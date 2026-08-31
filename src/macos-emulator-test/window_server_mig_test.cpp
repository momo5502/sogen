#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_display_state.hpp>
#include <gui/macos_event_stream.hpp>
#include <gui/skylight_routines.hpp>
#include <gui/macos_window_server_mig.hpp>
#include <mach/mig_kernel_servers.hpp>

#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    constexpr port_name_t REPLY_PORT = 0x70b;

    mig_server_table& window_server_table()
    {
        static mig_server_table table = [] {
            mig_server_table built{};
            sogen::register_window_server_mig_routines(built);
            return built;
        }();

        return table;
    }

    msg_call make_call(const port_name_t remote, const int32_t id, const uint32_t descriptors = 0)
    {
        msg_call call{};
        call.header = {.bits = (descriptors != 0 ? BITS_COMPLEX : 0u) | make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = static_cast<uint32_t>(MSG_HEADER_SIZE),
                       .remote_port = remote,
                       .local_port = REPLY_PORT,
                       .voucher_port = 0,
                       .id = id};
        call.descriptor_count = descriptors;
        return call;
    }

    std::vector<uint8_t> invoke(sogen::macos_emulator& emu, const kernel_object_kind kind, const int32_t id,
                                const std::vector<uint8_t>& body, const uint32_t descriptors = 0)
    {
        const auto* routine = window_server_table().find(kind, id);
        EXPECT_NE(routine, nullptr) << "no routine " << id << " registered";
        if (routine == nullptr)
        {
            return {};
        }

        const auto call = make_call(0x2f01, id, descriptors);
        return (*routine)(emu, make_mig_request(call, body, kind));
    }

    // The exact request SLSNewConnection sends: two port descriptors (the client-constructed event port
    // as make-send, the task identity token as copy-send), the NDR record, a zero, an empty name, and the
    // four trailing words whose last one is the CAContext id.
    std::vector<uint8_t> make_new_connection_body(const port_name_t event_port, const port_name_t identity, const uint32_t ca_context)
    {
        std::vector<uint8_t> body(MSG_BODY_SIZE + 2 * PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE + 6 * sizeof(uint32_t), 0);
        write_u32(body, 0, 2);
        write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE),
                              {.name = event_port, .disposition = disposition::make_send, .type = descriptor_type::port});
        write_port_descriptor(std::span{body}.subspan(MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE),
                              {.name = identity, .disposition = disposition::copy_send, .type = descriptor_type::port});

        const auto ndr = MSG_BODY_SIZE + 2 * PORT_DESCRIPTOR_SIZE;
        std::ranges::copy(NDR_RECORD, body.begin() + static_cast<ptrdiff_t>(ndr));

        const auto args = ndr + NDR_RECORD_SIZE;
        write_u32(body, args + 4, 1);
        write_u32(body, args + 5 * sizeof(uint32_t), ca_context);
        return body;
    }

    port_name_t descriptor_name(const std::vector<uint8_t>& reply, const size_t index)
    {
        return read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE + index * PORT_DESCRIPTOR_SIZE)).name;
    }

    uint8_t descriptor_disposition(const std::vector<uint8_t>& reply, const size_t index)
    {
        return read_port_descriptor(std::span{reply}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE + index * PORT_DESCRIPTOR_SIZE)).disposition;
    }

    // SLSNewConnection type-checks this reply harder than any other routine on the path: complex,
    // msgh_size exactly 64, no remote port, descriptor count 2, and both descriptors received as
    // move-send. Anything else and it returns -300 and the guest has no connection at all.
    TEST(WindowServerMig, NewConnectionPortAnswersTheShapeSlsNewConnectionAccepts)
    {
        const auto emu = macos_test::make_emulator();
        const auto event_port = emu->mach.ports.allocate_receive_right();
        const auto identity = emu->mach.ports.allocate_receive_right();

        const auto reply = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT,
                                  make_new_connection_body(event_port, identity, 0xC0533F68), 2);

        ASSERT_EQ(reply.size(), 64u);
        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, sogen::MACOS_MIG_NEW_CONNECTION_PORT + subsystem::reply_offset);
        EXPECT_EQ(header.size, 64u);
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 2u);
        EXPECT_EQ(descriptor_disposition(reply, 0), disposition::move_send);
        EXPECT_EQ(descriptor_disposition(reply, 1), disposition::move_send);
        EXPECT_TRUE(std::equal(NDR_RECORD.begin(), NDR_RECORD.end(), reply.begin() + 0x34));

        const auto connection = read_u32(reply, 0x3c);
        EXPECT_EQ(connection, sogen::MACOS_MAIN_CONNECTION_ID) << "the first connection is the main one";

        const auto* record = emu->ui.server.find_connection(connection);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->port, descriptor_name(reply, 0));
        EXPECT_EQ(record->shmem_entry, descriptor_name(reply, 1));
        EXPECT_EQ(record->ca_context, 0xC0533F68u);
    }

    // The whole reason this routine is on the input critical path: the request carries the port the
    // client built with mach_port_construct, and nothing else ever tells sogen what it is.
    TEST(WindowServerMig, NewConnectionPortAdoptsTheClientsEventPort)
    {
        const auto emu = macos_test::make_emulator();
        emu->ui.events.install(*emu);

        const auto event_port = emu->mach.ports.allocate_receive_right();
        const auto reply = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT,
                                  make_new_connection_body(event_port, emu->mach.ports.allocate_receive_right(), 0), 2);

        const auto connection = read_u32(reply, 0x3c);
        EXPECT_EQ(emu->ui.events.event_port_of(connection), event_port);
        EXPECT_EQ(emu->ui.events.connection_of_port(event_port), connection);
    }

    TEST(WindowServerMig, NewConnectionPortRefusesARequestWithNoEventPort)
    {
        const auto emu = macos_test::make_emulator();

        const std::vector<uint8_t> body(NDR_RECORD_SIZE + 4, 0);
        const auto reply = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT, body);

        ASSERT_EQ(reply.size(), MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(read_u32(reply, 32)), kr::invalid_argument);
        EXPECT_TRUE(emu->ui.server.connections().empty());
    }

    // The connection port is what the 30xxx traffic and the datagram pull travel on, so it has to be
    // resolvable back to the connection it belongs to.
    TEST(WindowServerMig, TheConnectionPortNamesItsConnection)
    {
        const auto emu = macos_test::make_emulator();
        const auto reply =
            invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT,
                   make_new_connection_body(emu->mach.ports.allocate_receive_right(), emu->mach.ports.allocate_receive_right(), 0), 2);

        const auto port = descriptor_name(reply, 0);
        const auto* record = emu->ui.server.connection_for_port(port);
        ASSERT_NE(record, nullptr);
        EXPECT_EQ(record->id, read_u32(reply, 0x3c));
        EXPECT_EQ(emu->mach.ports.object_of(port).kind, kernel_object_kind::window_server_connection);
        EXPECT_EQ(emu->ui.server.connection_for_port(PORT_NULL), nullptr);
    }

    // The two shmem routines hand over memory entries, and the guest immediately maps them. An entry
    // that no mach_vm_map can turn into an address is worse than no entry at all: the client treats the
    // failed map as fatal.
    TEST(WindowServerMig, TheShmemRoutinesHandOverMappableEntries)
    {
        const auto emu = macos_test::make_emulator();

        const auto events = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_EVENT_SHMEM, {});
        ASSERT_EQ(events.size(), 52u) << "measured: header, body and two port descriptors, no NDR";
        EXPECT_EQ(read_u32(events, MSG_HEADER_SIZE), 2u);

        const auto displays = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DISPLAY_SHMEM, {});
        ASSERT_EQ(displays.size(), 64u) << "measured: three port descriptors";
        EXPECT_EQ(read_u32(displays, MSG_HEADER_SIZE), 3u);

        const std::pair<const std::vector<uint8_t>*, std::vector<uint64_t>> groups[] = {
            {&events, {sogen::MACOS_EVENT_SCOREBOARD_BYTES, sogen::MACOS_EVENT_SCOREBOARD_AUX_BYTES}},
            {&displays,
             {sogen::MACOS_DISPLAY_SHMEM_HEADER_BYTES, sogen::MACOS_DISPLAY_SHMEM_STATE_BYTES, sogen::MACOS_DISPLAY_SHMEM_MODES_BYTES}},
        };

        for (const auto& [reply, sizes] : groups)
        {
            for (size_t i = 0; i < sizes.size(); ++i)
            {
                const auto name = descriptor_name(*reply, i);
                const auto* entry = emu->mach.find_memory_entry(name);
                ASSERT_NE(entry, nullptr) << "descriptor " << i << " is not a memory entry sogen made";
                EXPECT_GE(entry->size, sizes.at(i)) << "the guest maps this many bytes of it";

                std::vector<uint8_t> read(sizes.at(i), 0xAA);
                ASSERT_TRUE(emu->memory.try_read_memory(entry->address, read.data(), read.size()));
                EXPECT_EQ(std::ranges::count(read, 0), static_cast<ptrdiff_t>(read.size())) << "a fresh shared page is zeroed";
            }
        }

        EXPECT_EQ(emu->ui.server.scoreboard_address, emu->mach.find_memory_entry(descriptor_name(events, 0))->address);
    }

    TEST(WindowServerMig, TheShmemRoutinesHandOverTheSameEntriesEveryTime)
    {
        const auto emu = macos_test::make_emulator();

        const auto first = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_EVENT_SHMEM, {});
        const auto ports_after_first = emu->mach.ports.live_port_count();
        const auto second = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_EVENT_SHMEM, {});

        EXPECT_EQ(descriptor_name(first, 0), descriptor_name(second, 0));
        EXPECT_EQ(descriptor_name(first, 1), descriptor_name(second, 1));
        EXPECT_EQ(emu->mach.ports.live_port_count(), ports_after_first) << "the second call allocates no page and no port";
    }

    // GetDisplaySystemState carries a generation the client already has and answers with everything
    // newer, out of line. sogen's configuration never changes, so a request carrying its generation gets
    // the empty answer -- which still has to be the exact 56-byte shape the client's stub checks.
    TEST(WindowServerMig, GetDisplaySystemStateAnswersTheEmptyOutOfLineShapeForACurrentGeneration)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DISPLAY_SYSTEM_STATE,
                                  macos_test::ndr_body({static_cast<uint32_t>(sogen::MACOS_DISPLAY_STATE_GENERATION), 0}));

        ASSERT_EQ(reply.size(), 56u);
        const auto header = read_msg_header(reply);
        EXPECT_EQ(header.id, sogen::MACOS_MIG_GET_DISPLAY_SYSTEM_STATE + subsystem::reply_offset);
        EXPECT_NE(header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply, 0x18), 1u);

        const auto descriptor = read_ool_descriptor(std::span{reply}.subspan(0x1c));
        EXPECT_EQ(descriptor.type, descriptor_type::ool);
        EXPECT_EQ(descriptor.address, 0u);
        EXPECT_EQ(descriptor.size, 0u);
        EXPECT_EQ(descriptor.deallocate, 1u);
        EXPECT_TRUE(std::equal(NDR_RECORD.begin(), NDR_RECORD.end(), reply.begin() + 0x2c));
        EXPECT_EQ(read_u32(reply, 0x34), 0u) << "the count is repeated and the client checks the two agree";
    }

    // A client that knows nothing yet sends generation 0 and has to get the whole display configuration,
    // because the SkyLight side answers every display, mode and UUID query out of what it decodes here.
    TEST(WindowServerMig, GetDisplaySystemStatePublishesTheDesktopToAClientWithNoGeneration)
    {
        const auto emu = macos_test::make_emulator();
        emu->ui.desktop_width = 800;
        emu->ui.desktop_height = 600;

        const auto reply =
            invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DISPLAY_SYSTEM_STATE, macos_test::ndr_body({0, 0}));

        ASSERT_EQ(reply.size(), 56u);
        const auto descriptor = read_ool_descriptor(std::span{reply}.subspan(0x1c));
        ASSERT_NE(descriptor.address, 0u);
        ASSERT_NE(descriptor.size, 0u);
        EXPECT_EQ(read_u32(reply, 0x34), descriptor.size);

        std::vector<uint8_t> payload(descriptor.size, 0);
        ASSERT_TRUE(emu->memory.try_read_memory(descriptor.address, payload.data(), payload.size()));

        const sogen::macos_display_description expected{.id = sogen::MACOS_MAIN_DISPLAY_ID, .width = 800.0, .height = 600.0};
        EXPECT_EQ(payload, sogen::macos_build_display_system_state(std::span{&expected, 1}, sogen::MACOS_DISPLAY_STATE_GENERATION));
    }

    TEST(WindowServerMig, TheStatusOnlyRoutinesAnswerTheMeasuredSizes)
    {
        const auto emu = macos_test::make_emulator();

        const auto interests = invoke(*emu, kernel_object_kind::window_server_connection, sogen::MACOS_MIG_SET_PROCESS_NOTIFY_INTERESTS,
                                      macos_test::ndr_body({0, 2, 0xc8, 0x2bd}));
        ASSERT_EQ(interests.size(), 36u);
        EXPECT_EQ(read_u32(interests, MSG_HEADER_SIZE + NDR_RECORD_SIZE), static_cast<uint32_t>(kr::success));

        const auto mapping = invoke(*emu, kernel_object_kind::window_server_connection, sogen::MACOS_MIG_GET_UNIFIED_KEY_MAPPING, {});
        ASSERT_EQ(mapping.size(), 40u);
        EXPECT_EQ(read_u32(mapping, MSG_HEADER_SIZE + NDR_RECORD_SIZE), static_cast<uint32_t>(kr::success));

        const auto options = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DEBUG_OPTIONS, {});
        ASSERT_EQ(options.size(), 40u);
        EXPECT_EQ(read_u32(options, MSG_HEADER_SIZE + NDR_RECORD_SIZE), static_cast<uint32_t>(kr::success));
        EXPECT_EQ(read_u32(options, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 0u);
    }

    TEST(WindowServerMig, TheSessionHandshakeRoutinesHandOverTheSessionPort)
    {
        const auto emu = macos_test::make_emulator();

        const auto session = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_SESSION_PORT, {});
        ASSERT_EQ(session.size(), 40u);
        const auto port = descriptor_name(session, 0);
        EXPECT_EQ(port, emu->ui.server.server_port);
        EXPECT_EQ(descriptor_disposition(session, 0), disposition::move_send);

        const auto version = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_GET_CORE_GRAPHICS_SERVER_VERSION, {});
        ASSERT_EQ(version.size(), 60u);
        EXPECT_EQ(descriptor_name(version, 0), port) << "the handshake answers with the same session port";
        EXPECT_EQ(read_u32(version, MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE + NDR_RECORD_SIZE),
                  sogen::MACOS_CG_SERVER_VERSION);

        const auto death = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_SESSION_DEATH_WATCH_PORT, {});
        ASSERT_EQ(death.size(), 40u);
        EXPECT_NE(descriptor_name(death, 0), PORT_NULL);
        EXPECT_EQ(descriptor_name(death, 0), emu->ui.server.session_death_watch_port);
    }

    // _CASRegisterClient recomputes msgh_size from the server UUID length it is given and refuses the
    // reply when they disagree, so the empty-UUID answer has to be exactly 80 bytes.
    TEST(WindowServerMig, RegisterClientAnswersTheSizeCasRegisterClientRecomputes)
    {
        const auto emu = macos_test::make_emulator();

        const auto reply = invoke(*emu, kernel_object_kind::render_server, sogen::MACOS_MIG_CA_REGISTER_CLIENT, macos_test::ndr_body({0}));

        ASSERT_EQ(reply.size(), sogen::MACOS_CA_REGISTER_CLIENT_REPLY_BYTES);
        EXPECT_EQ(read_u32(reply, MSG_HEADER_SIZE), 2u);
        EXPECT_EQ(descriptor_disposition(reply, 0), disposition::move_send);
        EXPECT_EQ(descriptor_disposition(reply, 1), disposition::move_send);

        const auto uuid_length = read_u32(reply, 0x48);
        EXPECT_EQ(uuid_length, 0u);
        EXPECT_EQ(reply.size(), 0x50u + ((uuid_length + 3) & ~3u)) << "the size check inside _CASRegisterClient";

        const auto context_id = read_u32(reply, 0x3c);
        EXPECT_NE(context_id, 0u) << "a zero CAContext id reads as no context at all";
        EXPECT_EQ(read_u32(reply, 0x40), sogen::MACOS_CA_SESSION_ID);
        EXPECT_EQ(emu->mach.ports.object_of(descriptor_name(reply, 0)).kind, kernel_object_kind::render_server);
    }

    // The point of answering the bring-up at the MIG layer: a click reaches the guest. The ping lands on
    // the port the client constructed and shipped in the 32000 request -- a port sogen could not have
    // known about while SLSNewConnection was intercepted -- and the datagram comes back through a pull on
    // the connection port the same reply handed out.
    TEST(WindowServerMig, AClickReachesTheClientsOwnEventPortAndPullsBackThroughTheConnectionPort)
    {
        const auto emu = macos_test::make_emulator();
        emu->ui.events.install(*emu);

        const auto event_port = emu->mach.ports.allocate_receive_right();
        const auto reply = invoke(*emu, kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT,
                                  make_new_connection_body(event_port, emu->mach.ports.allocate_receive_right(), 0), 2);

        const auto connection = read_u32(reply, 0x3c);
        const auto connection_port = descriptor_name(reply, 0);

        auto* window = emu->ui.server.create_window(connection, 300, 200, 320, 232);
        ASSERT_NE(window, nullptr);

        const sogen::ui_event click{.window = window->id, .message = WM_LBUTTONDOWN, .wParam = 0, .lParam = (132u << 16) | 160u};
        ASSERT_TRUE(sogen::macos_translate_ui_event(*emu, click));

        const auto* parked = emu->mach.ports.destination_of(event_port);
        ASSERT_NE(parked, nullptr);
        ASSERT_EQ(parked->queue.size(), 1u) << "the datagram-available ping";
        const auto ping = read_msg_header(parked->queue.front());
        EXPECT_EQ(ping.id, 0);
        EXPECT_EQ(ping.size, MSG_HEADER_SIZE);
        EXPECT_EQ(ping.local_port, event_port);

        const auto* pull =
            window_server_table().find(kernel_object_kind::window_server_connection, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE);
        ASSERT_NE(pull, nullptr);

        const auto call = make_call(connection_port, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE);
        const std::vector<uint8_t> empty{};
        const auto stream_reply = (*pull)(*emu, make_mig_request(call, empty, kernel_object_kind::window_server_connection));

        ASSERT_EQ(stream_reply.size(), 56u);
        const auto descriptor = read_ool_descriptor(std::span{stream_reply}.subspan(0x1c));
        ASSERT_GT(descriptor.size, 8u) << "the click record travelled out of line";
        ASSERT_NE(descriptor.address, 0u);

        std::vector<uint8_t> stream(descriptor.size, 0);
        ASSERT_TRUE(emu->memory.try_read_memory(descriptor.address, stream.data(), stream.size()));
        EXPECT_EQ(read_u32(stream, 0), sogen::MACOS_DATAGRAM_EVENT_RECORD);
        EXPECT_EQ(read_u32(stream, 4) + 8, stream.size());
    }

    TEST(WindowServerMig, EveryBringUpRoutineIsRegisteredOnThePortKindItArrivesOn)
    {
        const auto& table = window_server_table();

        const std::pair<kernel_object_kind, int32_t> expected[] = {
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_CORE_GRAPHICS_SERVER_VERSION},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_SESSION_DEATH_WATCH_PORT},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_SESSION_PORT},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_NEW_CONNECTION_PORT},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DEBUG_OPTIONS},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_EVENT_SHMEM},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DISPLAY_SYSTEM_STATE},
            {kernel_object_kind::window_server, sogen::MACOS_MIG_GET_DISPLAY_SHMEM},
            {kernel_object_kind::window_server_connection, sogen::MACOS_MIG_SET_PROCESS_NOTIFY_INTERESTS},
            {kernel_object_kind::window_server_connection, sogen::MACOS_MIG_SET_CONNECTION_NOTIFY_INTERESTS},
            {kernel_object_kind::window_server_connection, sogen::MACOS_MIG_GET_DEVICES},
            {kernel_object_kind::window_server_connection, sogen::MACOS_MIG_GET_UNIFIED_KEY_MAPPING},
            {kernel_object_kind::window_server_connection, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE},
            {kernel_object_kind::render_server, sogen::MACOS_MIG_CA_REGISTER_CLIENT},
        };

        for (const auto& [kind, id] : expected)
        {
            EXPECT_NE(table.find(kind, id), nullptr) << "routine " << id << " is not registered";
            EXPECT_FALSE(table.name_of(kind, id).empty()) << "routine " << id << " has no name to report";
        }

        // The connection traffic must not answer on the root port: a 30xxx arriving there means the guest
        // used a port it was never handed, and that has to report itself rather than succeed.
        EXPECT_EQ(table.find(kernel_object_kind::window_server, sogen::MACOS_MIG_SET_PROCESS_NOTIFY_INTERESTS), nullptr);
        EXPECT_EQ(table.find(kernel_object_kind::window_server_connection, sogen::MACOS_MIG_NEW_CONNECTION_PORT), nullptr);
    }
}
