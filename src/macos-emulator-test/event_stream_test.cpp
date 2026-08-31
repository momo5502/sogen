#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_event_stream.hpp>
#include <stop_reason.hpp>
#include <macos_launch_target.hpp>
#include <gui/macos_layer_tree.hpp>
#include <gui/macos_ui_state.hpp>
#include <screenshot_ui_backend.hpp>
#include <mach/mig_kernel_servers.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    // The first 80 bytes of a right-mouse-down record captured on 25G76 with
    // event_datagram_handler under lldb: version prefix, then the eight fields every record starts
    // with. Click at global (460, 929) into a window at (300, 797) whose number is 20951.
 // /tmp/inp/dg8.log DGRAM 7;
    constexpr std::string_view MEASURED_PREFIX = "00000003"
                                                 "35400100"
                                                 "03000000"
                                                 "36400100"
                                                 "00000000"
                                                 "37400100"
                                                 "03000000"
                                                 "38c00200"
                                                 "43e60000"
                                                 "44684000"
                                                 "39c00200"
                                                 "43200000"
                                                 "43040000"
                                                 "3a000100"
                                                 "cf52bcfc6a640000"
                                                 "3b400100"
                                                 "00000000"
                                                 "33400100"
                                                 "d7510000";

    std::string to_hex(const std::span<const uint8_t> bytes)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out{};
        out.reserve(bytes.size() * 2);
        for (const auto byte : bytes)
        {
            out.push_back(digits[byte >> 4]);
            out.push_back(digits[byte & 0xF]);
        }
        return out;
    }

    struct tlv_field
    {
        uint16_t field{};
        uint16_t count{};
        std::vector<uint8_t> payload{};
    };

    // The framing rule the spec settles: [u16 field][u16 count], and the payload size follows from the
    // field number alone. Written out here rather than shared with the encoder, so a change to one is
    // not silently mirrored in the other.
    std::vector<tlv_field> parse_record(const std::span<const uint8_t> record, bool& complete)
    {
        std::vector<tlv_field> fields{};
        complete = false;

        if (record.size() < 4 || record[0] != 0 || record[1] != 0 || record[2] != 0 || record[3] != 3)
        {
            return fields;
        }

        size_t offset = 4;
        while (offset + 4 <= record.size())
        {
            const auto tag = read_u32(record, offset);
            offset += 4;

            const auto field = static_cast<uint16_t>(tag & 0xFFFFu);
            const auto count = static_cast<uint16_t>(tag >> 16);
            const size_t element = (field & 0x1000u) != 0 ? 1 : ((field & 0x4000u) != 0 ? 4 : 8);
            const auto size = static_cast<size_t>(count) * element;

            if (offset + size > record.size())
            {
                return fields;
            }

            fields.push_back(tlv_field{
                .field = field,
                .count = count,
                .payload = std::vector<uint8_t>(record.begin() + static_cast<ptrdiff_t>(offset),
                                                record.begin() + static_cast<ptrdiff_t>(offset + size)),
            });

            offset += size;
        }

        complete = offset == record.size();
        return fields;
    }

    std::map<uint16_t, uint64_t> scalar_fields(const std::span<const uint8_t> record)
    {
        bool complete = false;
        std::map<uint16_t, uint64_t> values{};

        for (const auto& field : parse_record(record, complete))
        {
            if (field.payload.size() == 4)
            {
                values[field.field] = read_u32(field.payload, 0);
            }
            else if (field.payload.size() == 8 && (field.field & 0x4000u) == 0)
            {
                values[field.field] = read_u64(field.payload, 0);
            }
        }

        return values;
    }

    float big_endian_float_at(const std::span<const uint8_t> bytes, const size_t offset)
    {
        const uint32_t raw = (static_cast<uint32_t>(bytes[offset]) << 24) | (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
                             (static_cast<uint32_t>(bytes[offset + 2]) << 8) | static_cast<uint32_t>(bytes[offset + 3]);
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    std::vector<uint8_t> point_payload(const std::span<const uint8_t> record, const uint16_t field)
    {
        bool complete = false;
        for (const auto& entry : parse_record(record, complete))
        {
            if (entry.field == field)
            {
                return entry.payload;
            }
        }
        return {};
    }

    sogen::macos_event_record measured_click()
    {
        return sogen::macos_event_record{
            .kind = sogen::macos_event_kind::right_mouse_down,
            .global_x = 460.0f,
            .global_y = 929.0f,
            .window_x = 160.0f,
            .window_y = 132.0f,
            .timestamp = 0x0000646afcbc52cfULL,
            .flags = 0,
            .window = 20951,
            .connection = 698099,
            .poster_pid = 93982,
            .window_height = 232,
            .screen_height = 1329,
            .button = 1,
            .button_down = true,
            .click_count = 2,
        };
    }

    TEST(EventStream, EncodesTheMeasuredCommonPrefixByteForByte)
    {
        const auto record = sogen::macos_encode_event_record(measured_click());

        ASSERT_GE(record.size(), MEASURED_PREFIX.size() / 2);
        EXPECT_EQ(to_hex(std::span{record}.first(MEASURED_PREFIX.size() / 2)), MEASURED_PREFIX)
            << "the version prefix and the eight leading fields are copied from a 25G76 capture";
    }

    TEST(EventStream, EveryFieldParsesWithTheMeasuredFramingRule)
    {
        for (const auto kind : {sogen::macos_event_kind::left_mouse_down, sogen::macos_event_kind::mouse_moved,
                                sogen::macos_event_kind::scroll_wheel, sogen::macos_event_kind::key_down})
        {
            auto source = measured_click();
            source.kind = kind;

            const auto record = sogen::macos_encode_event_record(source);
            bool complete = false;
            const auto fields = parse_record(record, complete);

            EXPECT_TRUE(complete) << "kind " << static_cast<uint32_t>(kind) << " left trailing bytes the framing cannot account for";
            EXPECT_FALSE(fields.empty());
        }
    }

    TEST(EventStream, MouseButtonRecordCarriesTheMeasuredButtonFields)
    {
        const auto down = scalar_fields(sogen::macos_encode_event_record(measured_click()));

        EXPECT_EQ(down.at(0x4037), 3u) << "kCGSEventRightMouseDown";
        EXPECT_EQ(down.at(0x4001), 1u);
        EXPECT_EQ(down.at(0x4002), 255u) << "pressure x 255 on a button-down";
        EXPECT_EQ(down.at(0x4003), 1u) << "button number: 0 left, 1 right, 2 other";
        EXPECT_EQ(down.at(0x405b), 20951u);
        EXPECT_EQ(down.at(0x405c), 20951u);
        EXPECT_EQ(down.at(0x406c), 2u);
        EXPECT_EQ(down.at(0x4034), 698099u);
        EXPECT_EQ(down.at(0x4055), 698099u) << "the connection id appears twice in every captured record";
        EXPECT_EQ(down.at(0x4029), 93982u);
        EXPECT_EQ(down.at(0x406a), 232u);
        EXPECT_EQ(down.at(0x406b), 1329u);
        EXPECT_EQ(down.at(0x003a), 0x0000646afcbc52cfULL);

        auto release = measured_click();
        release.kind = sogen::macos_event_kind::right_mouse_up;
        release.button_down = false;
        release.click_count = 0;

        const auto up = scalar_fields(sogen::macos_encode_event_record(release));
        EXPECT_EQ(up.at(0x4001), 0u);
        EXPECT_EQ(up.at(0x4002), 0u);
        EXPECT_EQ(up.at(0x4003), 1u) << "the button number survives the release";
    }

    TEST(EventStream, ScrollRecordCarriesTheMeasuredDeltaTriplets)
    {
        auto source = measured_click();
        source.kind = sogen::macos_event_kind::scroll_wheel;
        source.scroll_axis1 = 3;
        source.scroll_axis2 = -4;

        const auto fields = scalar_fields(sogen::macos_encode_event_record(source));

        EXPECT_EQ(fields.at(0x400b), 3u);
        EXPECT_EQ(fields.at(0x400c), 0xFFFFFFFCu);
        EXPECT_EQ(fields.at(0x400d), 0u);
        EXPECT_EQ(fields.at(0x405d), 0x00030000u) << "16.16 fixed point, measured";
        EXPECT_EQ(fields.at(0x405e), 0xFFFC0000u);
        EXPECT_EQ(fields.at(0x4060), 30u) << "ten points per line, measured";
        EXPECT_EQ(fields.at(0x4061), 0xFFFFFFD8u);

        EXPECT_EQ(fields.count(0x4001), 0u) << "a scroll carries no button fields";
    }

    TEST(EventStream, KeyRecordCarriesTheMeasuredKeyFields)
    {
        auto source = measured_click();
        source.kind = sogen::macos_event_kind::key_down;
        source.keycode = 5;
        source.character = 'g';
        source.window = 0;
        source.window_x = 0.0f;
        source.window_y = 0.0f;

        const auto record = sogen::macos_encode_event_record(source);
        const auto fields = scalar_fields(record);

        EXPECT_EQ(fields.at(0x4037), 10u);
        EXPECT_EQ(fields.at(0x4009), 5u);
        EXPECT_EQ(fields.at(0x4008), 0u);
        EXPECT_EQ(fields.at(0x400a), sogen::MACOS_EVENT_KEYBOARD_ID);
        EXPECT_EQ(fields.at(0x404c), 0xfcu);
        EXPECT_EQ(fields.at(0x404d), static_cast<uint32_t>('g'));
        EXPECT_EQ(fields.at(0x404e), static_cast<uint32_t>('g'));
        EXPECT_EQ(fields.at(0x4052), static_cast<uint32_t>('g'));
        EXPECT_EQ(fields.at(0x4050), 10u) << "measured as 10 on key-up too, so it is not the event type";
        EXPECT_EQ(fields.at(0x4033), 0u) << "a key record names no window";

        const auto window_point = point_payload(record, 0xc039);
        ASSERT_EQ(window_point.size(), 8u);
        EXPECT_EQ(big_endian_float_at(window_point, 0), 0.0f);
        EXPECT_EQ(big_endian_float_at(window_point, 4), 0.0f);
    }

    TEST(EventStream, PointsAreTwoBigEndianFloats)
    {
        const auto record = sogen::macos_encode_event_record(measured_click());

        const auto global = point_payload(record, 0xc038);
        ASSERT_EQ(global.size(), 8u);
        EXPECT_EQ(big_endian_float_at(global, 0), 460.0f);
        EXPECT_EQ(big_endian_float_at(global, 4), 929.0f);
    }

    TEST(EventStream, WrapsARecordInTheMeasuredDatagramFraming)
    {
        const auto payload = sogen::macos_encode_event_record(measured_click());
        const auto datagram = sogen::macos_encode_datagram(sogen::MACOS_DATAGRAM_EVENT_RECORD, payload);

        ASSERT_EQ(datagram.size(), payload.size() + 8);
        EXPECT_EQ(read_u32(datagram, 0), sogen::MACOS_DATAGRAM_EVENT_RECORD);
        EXPECT_EQ(read_u32(datagram, 4), payload.size());
        EXPECT_TRUE(std::equal(payload.begin(), payload.end(), datagram.begin() + 8));
    }

    TEST(EventStream, AnEmptyStreamEncodesToNothing)
    {
        sogen::macos_event_stream stream{};
        EXPECT_EQ(stream.pending_bytes(0x1D01), 0u);
        EXPECT_TRUE(stream.take_stream(0x1D01).empty());
    }

    struct stream_fixture
    {
        std::unique_ptr<sogen::macos_emulator> emu{macos_test::make_emulator()};
        sogen::macos_event_stream stream{};
        port_name_t event_port{};
        uint32_t connection{};

        void prepare(const uint32_t id)
        {
            this->connection = id;
            this->event_port = this->emu->mach.ports.allocate_receive_right({.kind = kernel_object_kind::window_server_event, .id = id});
            this->stream.install(*this->emu);
            this->stream.adopt_event_port(*this->emu, id, this->event_port);
        }

        size_t queued() const
        {
            const auto* entry = this->emu->mach.ports.destination_of(this->event_port);
            return entry == nullptr ? 0 : entry->queue.size();
        }

        sogen::macos_event_record record() const
        {
            auto value = measured_click();
            value.connection = this->connection;
            return value;
        }
    };

    TEST(EventStream, PingsTheEventPortOnlyOnTheEmptyToNonEmptyEdge)
    {
        stream_fixture fixture{};
        fixture.prepare(0x2001);

        ASSERT_TRUE(fixture.stream.post(*fixture.emu, fixture.record()));
        ASSERT_TRUE(fixture.stream.post(*fixture.emu, fixture.record()));

        EXPECT_EQ(fixture.stream.ping_count(), 1u) << "the second record joins a stream that is already non-empty";
        ASSERT_EQ(fixture.queued(), 1u);

        const auto* entry = fixture.emu->mach.ports.destination_of(fixture.event_port);
        ASSERT_NE(entry, nullptr);
        const auto& ping = entry->queue.front();

        ASSERT_EQ(ping.size(), MSG_HEADER_SIZE) << "the datagram-available ping is a bare header";
        const auto header = read_msg_header(ping);
        EXPECT_EQ(header.id, 0);
        EXPECT_EQ(header.size, MSG_HEADER_SIZE);
        EXPECT_EQ(header.bits, 0x20111100u) << "measured on 25G76";
        EXPECT_EQ(header.remote_port, PORT_NULL);
        EXPECT_EQ(header.local_port, fixture.event_port);

        // Draining and posting again pings once more: the edge is what the server watches, not the
        // arrival.
        (void)fixture.stream.take_stream(fixture.connection);
        ASSERT_TRUE(fixture.stream.post(*fixture.emu, fixture.record()));
        EXPECT_EQ(fixture.stream.ping_count(), 2u);
    }

    TEST(EventStream, AnEventForAConnectionWithNoEventPortIsReportedByName)
    {
        stream_fixture fixture{};
        fixture.stream.install(*fixture.emu);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        auto orphan = measured_click();
        orphan.connection = 0x5AFE;
        EXPECT_FALSE(fixture.stream.post(*fixture.emu, orphan));

        EXPECT_NE(captured.find("no event port registered"), std::string::npos) << captured;
    }

    TEST(EventStream, AStreamPastItsLimitIsRefusedByName)
    {
        stream_fixture fixture{};
        fixture.prepare(0x2002);

        const auto record_bytes = sogen::macos_encode_event_record(fixture.record()).size() + 8;
        const auto fits = sogen::MACOS_EVENT_STREAM_MAX_BYTES / record_bytes;

        for (size_t i = 0; i < fits; ++i)
        {
            ASSERT_TRUE(fixture.stream.post(*fixture.emu, fixture.record())) << "record " << i;
        }

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(fixture.stream.post(*fixture.emu, fixture.record()));
        EXPECT_NE(captured.find("byte limit"), std::string::npos) << captured;
        EXPECT_LE(fixture.stream.pending_bytes(fixture.connection), sogen::MACOS_EVENT_STREAM_MAX_BYTES);
    }

    msg_call make_pull_call(const port_name_t remote)
    {
        msg_call call{};
        call.header = {.bits = make_bits(disposition::copy_send, disposition::make_send_once),
                       .size = static_cast<uint32_t>(MSG_HEADER_SIZE),
                       .remote_port = remote,
                       .local_port = 0x70b,
                       .voucher_port = 0,
                       .id = sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE};
        return call;
    }

    // Field offsets from the measured 30217 reply; see the spec's "The 30117 reply, corrected".
    struct pull_reply
    {
        msg_header header{};
        uint32_t descriptor_count{};
        ool_descriptor descriptor{};
        uint32_t trailing_size{};
        bool ndr_present{};
    };

    pull_reply decode_pull_reply(const std::vector<uint8_t>& reply)
    {
        pull_reply decoded{};
        if (reply.size() != 56)
        {
            return decoded;
        }

        decoded.header = read_msg_header(reply);
        decoded.descriptor_count = read_u32(reply, 0x18);
        decoded.descriptor = read_ool_descriptor(std::span{reply}.subspan(0x1c));
        decoded.ndr_present = std::equal(NDR_RECORD.begin(), NDR_RECORD.end(), reply.begin() + 0x2c);
        decoded.trailing_size = read_u32(reply, 0x34);
        return decoded;
    }

    TEST(EventStream, AnEmptyPullAnswersWithTheMeasuredZeroLengthReply)
    {
        stream_fixture fixture{};
        fixture.prepare(0x2003);

        const auto call = make_pull_call(fixture.event_port);
        const std::vector<uint8_t> body{};
        const auto reply = sogen::macos_event_stream_pull(*fixture.emu, make_mig_request(call, body, kernel_object_kind::window_server));

        ASSERT_EQ(reply.size(), 56u) << "the measured reply is exactly 56 bytes, drained or not";
        const auto decoded = decode_pull_reply(reply);

        EXPECT_EQ(decoded.header.id, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE_REPLY);
        EXPECT_EQ(decoded.header.size, 56u);
        EXPECT_EQ(decoded.header.bits, 0x80001200u) << "complex, move_send_once local, measured";
        EXPECT_EQ(decoded.header.remote_port, PORT_NULL);
        EXPECT_EQ(decoded.header.local_port, 0x70bu);
        EXPECT_EQ(decoded.descriptor_count, 1u);
        EXPECT_EQ(decoded.descriptor.address, 0u);
        EXPECT_EQ(decoded.descriptor.size, 0u);
        EXPECT_EQ(decoded.descriptor.deallocate, 1u) << "the host says deallocate even on the empty answer";
        EXPECT_EQ(decoded.descriptor.copy, 1u);
        EXPECT_EQ(decoded.descriptor.type, descriptor_type::ool);
        EXPECT_TRUE(decoded.ndr_present);
        EXPECT_EQ(decoded.trailing_size, 0u) << "the size is repeated, and the client checks the two agree";
    }

    TEST(EventStream, APullHandsOverEveryQueuedDatagramAndDrains)
    {
        stream_fixture fixture{};
        fixture.prepare(0x2004);

        ASSERT_TRUE(fixture.stream.post(*fixture.emu, fixture.record()));
        auto second = fixture.record();
        second.kind = sogen::macos_event_kind::right_mouse_up;
        second.button_down = false;
        ASSERT_TRUE(fixture.stream.post(*fixture.emu, second));

        const auto expected = fixture.stream.pending_bytes(fixture.connection);
        ASSERT_GT(expected, 0u);

        const auto call = make_pull_call(fixture.event_port);
        const std::vector<uint8_t> body{};
        const auto reply = sogen::macos_event_stream_pull(*fixture.emu, make_mig_request(call, body, kernel_object_kind::window_server));

        const auto decoded = decode_pull_reply(reply);
        ASSERT_EQ(decoded.descriptor.size, expected);
        EXPECT_EQ(decoded.trailing_size, expected);
        ASSERT_NE(decoded.descriptor.address, 0u);

        std::vector<uint8_t> stream(expected, 0);
        ASSERT_TRUE(fixture.emu->memory.try_read_memory(decoded.descriptor.address, stream.data(), stream.size()));

        // Two back-to-back datagrams, each [u32 type][u32 length][payload].
        ASSERT_GE(stream.size(), 8u);
        EXPECT_EQ(read_u32(stream, 0), sogen::MACOS_DATAGRAM_EVENT_RECORD);
        const auto first_length = read_u32(stream, 4);
        ASSERT_LE(8 + first_length + 8, stream.size());
        EXPECT_EQ(read_u32(stream, 8 + first_length), sogen::MACOS_DATAGRAM_EVENT_RECORD);
        const auto second_length = read_u32(stream, 8 + first_length + 4);
        EXPECT_EQ(8 + first_length + 8 + second_length, stream.size()) << "nothing but the two datagrams";

        const auto drained = sogen::macos_event_stream_pull(*fixture.emu, make_mig_request(call, body, kernel_object_kind::window_server));
        EXPECT_EQ(decode_pull_reply(drained).descriptor.size, 0u) << "the client repeats the pull until it gets an empty one";
    }

    TEST(EventStream, TheRegisteredRoutineIsBoundToBothWindowServerPortKinds)
    {
        mig_server_table table{};
        sogen::register_event_stream_routines(table);

        EXPECT_NE(table.find(kernel_object_kind::window_server, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE), nullptr);
        EXPECT_NE(table.find(kernel_object_kind::window_server_event, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE), nullptr);
        EXPECT_EQ(table.name_of(kernel_object_kind::window_server, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE), "GetPortStreamOutofline");
        EXPECT_EQ(table.find(kernel_object_kind::task, sogen::MACOS_MIG_GET_PORT_STREAM_OUTOFLINE), nullptr);
    }

    TEST(EventStream, AStreamStopsBeingReachableWhenItDies)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_EQ(sogen::installed_event_stream(*emu), nullptr);

        {
            sogen::macos_event_stream stream{};
            stream.install(*emu);
            EXPECT_EQ(sogen::installed_event_stream(*emu), &stream);
        }

        EXPECT_EQ(sogen::installed_event_stream(*emu), nullptr) << "a dead stream must not be reachable through a stale pointer";
    }

    struct translate_fixture : stream_fixture
    {
        sogen::macos_window* window{};

        void prepare_window(const int32_t x, const int32_t y, const int32_t width, const int32_t height)
        {
            this->emu->ui.desktop_width = 1440;
            this->emu->ui.desktop_height = 900;
            this->window = this->emu->ui.server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, x, y, width, height);
            this->prepare(sogen::MACOS_MAIN_CONNECTION_ID);
        }

        std::vector<uint8_t> last_record()
        {
            auto stream = this->stream.take_stream(this->connection);
            if (stream.size() < 8)
            {
                return {};
            }

            const auto length = read_u32(stream, 4);
            return std::vector<uint8_t>(stream.begin() + 8, stream.begin() + 8 + static_cast<ptrdiff_t>(length));
        }
    };

    uint64_t pack_client_point(const int16_t x, const int16_t y)
    {
        return (static_cast<uint64_t>(static_cast<uint16_t>(y)) << 16) | static_cast<uint16_t>(x);
    }

    TEST(EventStream, TranslatesAClickIntoWindowAndScreenCoordinates)
    {
        translate_fixture fixture{};
        fixture.prepare_window(300, 200, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        const sogen::ui_event click{
            .window = fixture.window->id,
            .message = WM_LBUTTONDOWN,
            .wParam = MK_LBUTTON,
            .lParam = pack_client_point(40, 60),
        };

        ASSERT_TRUE(sogen::macos_translate_ui_event(*fixture.emu, click));

        const auto record = fixture.last_record();
        ASSERT_FALSE(record.empty());

        const auto fields = scalar_fields(record);
        EXPECT_EQ(fields.at(0x4037), static_cast<uint32_t>(sogen::macos_event_kind::left_mouse_down));
        EXPECT_EQ(fields.at(0x4003), 0u) << "left button";
        EXPECT_EQ(fields.at(0x4001), 1u);
        EXPECT_EQ(fields.at(0x4033), fixture.window->id);
        EXPECT_EQ(fields.at(0x406a), 232u);
        EXPECT_EQ(fields.at(0x406b), 900u) << "the emulated desktop height, not the measurement host's";

        const auto window_point = point_payload(record, 0xc039);
        ASSERT_EQ(window_point.size(), 8u);
        EXPECT_EQ(big_endian_float_at(window_point, 0), 40.0f);
        EXPECT_EQ(big_endian_float_at(window_point, 4), 60.0f);

        const auto global = point_payload(record, 0xc038);
        ASSERT_EQ(global.size(), 8u);
        EXPECT_EQ(big_endian_float_at(global, 0), 340.0f) << "window origin plus the client point";
        EXPECT_EQ(big_endian_float_at(global, 4), 260.0f);
    }

    // The far end of the idle park: a guest with nothing runnable comes back only when something outside
    // it delivers, and "delivered" has to mean the event reached the guest's own event stream rather than
    // merely arriving at the backend. This is the browser's click path with the browser taken out of it.
    TEST(EventStream, AClickPostedFromOutsideEndsAParkedGuestsWait)
    {
        translate_fixture fixture{};
        fixture.prepare_window(300, 200, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        auto& emu = *fixture.emu;

        auto backend = sogen::create_screenshot_ui_backend();
        auto* const screen = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        screen->set_input_source(true);
        emu.set_ui_backend(std::move(backend));
        emu.ui.attach_input(emu);

        ASSERT_TRUE(emu.can_wake_from_host_input());
        ASSERT_EQ(emu.ui.delivered_input_count(), 0u);

        constexpr uint64_t svc = 0x100000000ULL;
        emu.emu().reg(sogen::arm64_register::pc, svc + 4);

        size_t polls = 0;
        emu.on_host_idle = [&] {
            ++polls;

            if (polls == 2)
            {
                screen->post_event(sogen::ui_event{
                    .window = fixture.window->id,
                    .message = WM_LBUTTONDOWN,
                    .wParam = MK_LBUTTON,
                    .lParam = pack_client_point(40, 60),
                });
            }

            if (polls > 50)
            {
                emu.stop();
            }
        };

        emu.park_for_host_input();

        EXPECT_EQ(emu.ui.delivered_input_count(), 1u) << "the click reached the guest, not just the backend";
        EXPECT_LE(polls, 3u) << "and the park ended on the click rather than on the stop above";
        EXPECT_EQ(emu.last_stop_reason(), sogen::stop_reason::none) << "nothing was stopped; the guest was woken";
        EXPECT_EQ(emu.emu().read_instruction_pointer(), svc) << "the waiter is left on its own svc, so its wait runs again";
    }

    // A backend may report a point outside the desktop: a window can be moved partly off-screen, and a
    // drag keeps reporting while the pointer is past the edge. The record has to carry it rather than
    // clamp, because the guest's own hit testing is what decides.
    TEST(EventStream, CarriesCoordinatesOutsideTheDesktopUnchanged)
    {
        translate_fixture fixture{};
        fixture.prepare_window(1300, 850, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        const sogen::ui_event drag{
            .window = fixture.window->id,
            .message = WM_MOUSEMOVE,
            .wParam = MK_LBUTTON,
            .lParam = pack_client_point(static_cast<int16_t>(-30), 300),
        };

        ASSERT_TRUE(sogen::macos_translate_ui_event(*fixture.emu, drag));

        const auto record = fixture.last_record();
        ASSERT_FALSE(record.empty());

        EXPECT_EQ(scalar_fields(record).at(0x4037), static_cast<uint32_t>(sogen::macos_event_kind::left_mouse_dragged))
            << "a move with a button held is a drag";

        const auto global = point_payload(record, 0xc038);
        ASSERT_EQ(global.size(), 8u);
        EXPECT_EQ(big_endian_float_at(global, 0), 1270.0f);
        EXPECT_EQ(big_endian_float_at(global, 4), 1150.0f) << "past the bottom of a 900 pixel desktop, and left that way";
    }

    TEST(EventStream, TracksModifiersFromTheModifierKeysThemselves)
    {
        translate_fixture fixture{};
        fixture.prepare_window(0, 0, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        const auto press = [&](const uint32_t message, const uint64_t vk) {
            return sogen::macos_translate_ui_event(*fixture.emu,
                                                   sogen::ui_event{.window = fixture.window->id, .message = message, .wParam = vk});
        };

        ASSERT_TRUE(press(WM_KEYDOWN, VK_SHIFT));
        ASSERT_TRUE(press(WM_KEYDOWN, 'G'));

        auto stream = fixture.stream.take_stream(fixture.connection);
        ASSERT_GE(stream.size(), 8u);
        const auto first_length = read_u32(stream, 4);
        const std::vector<uint8_t> second(stream.begin() + 8 + static_cast<ptrdiff_t>(first_length) + 8, stream.end());

        const auto fields = scalar_fields(second);
        EXPECT_EQ(fields.at(0x4009), 5u) << "kVK_ANSI_G";
        EXPECT_EQ(fields.at(0x403b), sogen::MACOS_CG_FLAG_SHIFT) << "measured as 0x20000 on a shifted key-down";
        EXPECT_EQ(fields.at(0x404d), static_cast<uint32_t>('G'));

        ASSERT_TRUE(press(WM_KEYUP, VK_SHIFT));
        ASSERT_TRUE(press(WM_KEYDOWN, 'G'));

        auto after = fixture.stream.take_stream(fixture.connection);
        const auto up_length = read_u32(after, 4);
        const std::vector<uint8_t> unshifted(after.begin() + 8 + static_cast<ptrdiff_t>(up_length) + 8, after.end());

        const auto plain = scalar_fields(unshifted);
        EXPECT_EQ(plain.at(0x403b), 0u);
        EXPECT_EQ(plain.at(0x404d), static_cast<uint32_t>('g'));
    }

    TEST(EventStream, AUiEventMessageWithNoRecordFormIsReportedByName)
    {
        translate_fixture fixture{};
        fixture.prepare_window(0, 0, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(sogen::macos_translate_ui_event(
            *fixture.emu, sogen::ui_event{.window = fixture.window->id, .message = WM_MOUSEHWHEEL, .wParam = 0, .lParam = 0}));

        EXPECT_NE(captured.find("WM_MOUSEHWHEEL"), std::string::npos) << captured;
        EXPECT_EQ(fixture.stream.pending_bytes(fixture.connection), 0u);
    }

    TEST(EventStream, AVirtualKeyWithNoMacosKeycodeIsReportedByName)
    {
        translate_fixture fixture{};
        fixture.prepare_window(0, 0, 320, 232);
        ASSERT_NE(fixture.window, nullptr);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(sogen::macos_translate_ui_event(
            *fixture.emu, sogen::ui_event{.window = fixture.window->id, .message = WM_KEYDOWN, .wParam = VK_SCROLL}));

        EXPECT_NE(captured.find("no macOS keycode"), std::string::npos) << captured;
    }

    struct desktop_input_fixture : stream_fixture
    {
        void prepare_desktop(const int32_t width, const int32_t height)
        {
            this->emu->ui.desktop_width = width;
            this->emu->ui.desktop_height = height;
            this->prepare(sogen::MACOS_MAIN_CONNECTION_ID);
        }

        // The id, not the pointer: a later create_window reallocates the server's vector and every
        // pointer taken before it dangles.
        uint32_t add_window(const int32_t x, const int32_t y, const int32_t w, const int32_t h)
        {
            auto* window = this->emu->ui.server.create_window(sogen::MACOS_MAIN_CONNECTION_ID, x, y, w, h);
            if (window == nullptr)
            {
                return 0;
            }

            window->ordered_in = true;
            return window->id;
        }

        std::vector<uint8_t> last_record()
        {
            auto stream = this->stream.take_stream(this->connection);
            if (stream.size() < 8)
            {
                return {};
            }

            const auto length = read_u32(stream, 4);
            return std::vector<uint8_t>(stream.begin() + 8, stream.begin() + 8 + static_cast<ptrdiff_t>(length));
        }
    };

    TEST(EventStream, ADesktopPointHitsTheWindowInFront)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);

        const auto back = fixture.add_window(100, 100, 200, 200);
        const auto front = fixture.add_window(150, 150, 200, 200);
        ASSERT_NE(back, 0u);
        ASSERT_NE(front, 0u);

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = 200, .screen_y = 200}));

        const auto fields = scalar_fields(fixture.last_record());
        EXPECT_EQ(fields.at(0x4033), front) << "the overlap belongs to the window drawn last";
        EXPECT_EQ(fields.at(0x406a), 200u);

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = 120, .screen_y = 120}));
        EXPECT_EQ(scalar_fields(fixture.last_record()).at(0x4033), back) << "outside the front window, the one behind it";

        fixture.emu->ui.server.find_window(front)->ordered_in = false;
        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = 200, .screen_y = 200}));
        EXPECT_EQ(scalar_fields(fixture.last_record()).at(0x4033), back) << "a window that is not ordered in catches nothing";
    }

    TEST(EventStream, ADesktopPointCarriesTheWindowRelativeOffset)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);
        ASSERT_NE(fixture.add_window(300, 200, 320, 232), 0u);

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = 340, .screen_y = 260}));

        const auto record = fixture.last_record();
        const auto window_point = point_payload(record, 0xc039);
        ASSERT_EQ(window_point.size(), 8u);
        EXPECT_EQ(big_endian_float_at(window_point, 0), 40.0f);
        EXPECT_EQ(big_endian_float_at(window_point, 4), 60.0f);

        const auto global = point_payload(record, 0xc038);
        ASSERT_EQ(global.size(), 8u);
        EXPECT_EQ(big_endian_float_at(global, 0), 340.0f);
        EXPECT_EQ(big_endian_float_at(global, 4), 260.0f);

        const auto fields = scalar_fields(record);
        EXPECT_EQ(fields.at(0x4001), 1u);
        EXPECT_EQ(fields.at(0x406c), 1u) << "one click";
        EXPECT_EQ(fields.at(0x406b), 480u);
    }

    TEST(EventStream, AMouseUpCarriesNoClickCountAndNoPressure)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);
        ASSERT_NE(fixture.add_window(0, 0, 320, 232), 0u);

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_up, .screen_x = 10, .screen_y = 10}));

        const auto fields = scalar_fields(fixture.last_record());
        EXPECT_EQ(fields.at(0x4001), 0u);
        EXPECT_EQ(fields.at(0x4002), 0u);
        EXPECT_EQ(fields.at(0x406c), 0u) << "measured: the host puts 0 in the click count of a mouse-up";
    }

    // Measured on 25G76: a key record carries window number 0, so the connection it belongs to cannot
    // come from a hit test. It is the frontmost visible window's owner.
    TEST(EventStream, AKeyEventGoesToTheFrontmostWindowsConnection)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::key_down, .keycode = 5, .character = 'g'}))
            << "no window is on screen, so no connection owns a keystroke";
        EXPECT_NE(captured.find("no connection owns it"), std::string::npos) << captured;

        ASSERT_NE(fixture.add_window(0, 0, 320, 232), 0u);
        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::key_down, .keycode = 5, .character = 'g'}));

        const auto record = fixture.last_record();
        const auto fields = scalar_fields(record);
        EXPECT_EQ(fields.at(0x4033), 0u) << "measured: a key record names no window";
        EXPECT_EQ(fields.at(0x4034), fixture.connection);
        EXPECT_EQ(fields.at(0x4009), 5u);
        EXPECT_EQ(fields.at(0x404d), static_cast<uint32_t>('g'));

        const auto window_point = point_payload(record, 0xc039);
        ASSERT_EQ(window_point.size(), 8u);
        EXPECT_EQ(big_endian_float_at(window_point, 0), 0.0f);
        EXPECT_EQ(big_endian_float_at(window_point, 4), 0.0f);
    }

    TEST(EventStream, APointOnNoWindowIsReportedByName)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);
        ASSERT_NE(fixture.add_window(0, 0, 100, 100), 0u);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = 500, .screen_y = 400}));
        EXPECT_NE(captured.find("lands on no window"), std::string::npos) << captured;
    }

    // Measured: SLSCurrentInputPointerPosition reads two little-endian f32 at the start of the first
    // event-shmem page and hands them straight back, so this is the cursor SLSGetCurrentCursorLocation
    // reports. A key event does not move the pointer and must not disturb it.
    TEST(EventStream, AMouseEventUpdatesTheScoreboardCursorAndAKeyEventDoesNot)
    {
        desktop_input_fixture fixture{};
        fixture.prepare_desktop(640, 480);
        ASSERT_NE(fixture.add_window(0, 0, 320, 232), 0u);

        constexpr uint64_t scoreboard = 0x360000000ULL;
        ASSERT_TRUE(fixture.emu->memory.allocate_memory(scoreboard, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write));
        fixture.emu->ui.server.scoreboard_address = scoreboard;

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::mouse_moved, .screen_x = 47, .screen_y = 129}));

        // Read at the literal offsets the disassembly gives, not through the constants, so a constant
        // that drifts off the measurement fails here.
        std::array<uint8_t, 0x28> page{};
        const auto reread = [&] { ASSERT_TRUE(fixture.emu->memory.try_read_memory(scoreboard, page.data(), page.size())); };

        reread();
        std::array<float, 2> cursor{};
        std::memcpy(cursor.data(), page.data() + 0x00, sizeof(cursor));
        EXPECT_EQ(cursor[0], 47.0f);
        EXPECT_EQ(cursor[1], 129.0f);

        uint64_t stamp = 0;
        std::memcpy(&stamp, page.data() + 0x18, sizeof(stamp));
        EXPECT_NE(stamp, 0u);

        EXPECT_TRUE(std::all_of(page.begin() + 0x08, page.begin() + 0x18, [](const uint8_t byte) { return byte == 0; }))
            << "nothing but the cursor and the timestamp is written";
        EXPECT_TRUE(std::all_of(page.begin() + 0x20, page.end(), [](const uint8_t byte) { return byte == 0; }));

        ASSERT_TRUE(sogen::macos_post_input_event(
            *fixture.emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::key_down, .keycode = 5, .character = 'g'}));

        reread();
        std::memcpy(cursor.data(), page.data() + 0x00, sizeof(cursor));
        EXPECT_EQ(cursor[0], 47.0f) << "a keystroke does not move the pointer";
        EXPECT_EQ(cursor[1], 129.0f);
    }

    std::string environment_value(const char* name)
    {
        const auto* value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string{value};
    }

    // The gate the whole delivery path exists for: a real AppKit app running inside the emulator, an
    // event injected the way a host front-end injects one, and the app's own -[NSApplication sendEvent:]
    // printing what it was handed. Disabled because it boots against the host's shared cache and runs
    // for minutes; src/tools/macos-gui-probe/README.md has the invocation.
    TEST(EventStream, DISABLED_AnInjectedEventReachesTheGuestsOwnAppKit)
    {
        const auto root = environment_value("SOGEN_MACOS_ROOT");
        const auto probe = environment_value("SOGEN_INPUT_PROBE");

        if (root.empty() || probe.empty() || !std::filesystem::exists(probe))
        {
            GTEST_SKIP() << "set SOGEN_MACOS_ROOT to an emulation root and SOGEN_INPUT_PROBE to the host path of the inputprobe binary";
        }

        const auto launch = sogen::resolve_macos_launch_target(std::filesystem::path{probe});
        ASSERT_TRUE(launch.runnable()) << launch.diagnostic;

        auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{root});

        auto backend = sogen::create_screenshot_ui_backend();
        auto* shot = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        shot->set_desktop_size(640, 480);
        emu->set_ui_backend(std::move(backend));

        emu->ui.enabled = true;
        emu->ui.desktop_width = 640;
        emu->ui.desktop_height = 480;

        std::string out{};
        emu->callbacks.on_stdout = [&](const std::string_view data) { out.append(data); };
        emu->log.disable_output(environment_value("SOGEN_INPUT_PROBE_VERBOSE").empty());

        sogen::apply_macos_launch_target(launch, emu->file_sys);
        emu->process.current_working_directory = launch.working_directory;

        std::vector<std::string> envp{"PATH=/usr/bin:/bin", "HOME=/var/root", "TMPDIR=/tmp"};
        if (!environment_value("INPUTPROBE_RAW").empty())
        {
            envp.emplace_back("INPUTPROBE_RAW=1");
        }

        ASSERT_TRUE(emu->load_executable(launch.guest_executable, {launch.guest_executable}, envp));

        constexpr size_t chunk = 200'000'000;
        constexpr size_t chunks_to_window = 300;

        const auto run = [&](const size_t chunks, const std::string_view until) {
            for (size_t i = 0; i < chunks && out.find(until) == std::string::npos; ++i)
            {
                emu->start(chunk);

                const auto reason = emu->last_stop_reason();
                if ((reason != sogen::stop_reason::none && reason != sogen::stop_reason::instruction_limit) ||
                    emu->process.exit_status.has_value())
                {
                    std::cout << "the guest stopped after " << emu->get_executed_instructions()
                              << " instructions: " << emu->last_stop_detail() << "\n";
                    return;
                }
            }
        };

        run(chunks_to_window, "INPUTPROBE-READY");
        ASSERT_NE(out.find("INPUTPROBE-READY"), std::string::npos) << out;

        // The probe prints as soon as makeKeyAndOrderFront: returns; the order-in reaches the server a
        // little later, and a click before it lands on nothing.
        uint32_t target = 0;
        for (size_t i = 0; i < 20 && target == 0; ++i)
        {
            for (const auto& candidate : emu->ui.server.windows())
            {
                if (candidate.ordered_in)
                {
                    target = candidate.id;
                }
            }

            if (target == 0)
            {
                emu->start(chunk);
            }
        }

        for (const auto& candidate : emu->ui.server.windows())
        {
            std::cout << "window " << candidate.id << " at (" << candidate.x << "," << candidate.y << ") " << candidate.width << "x"
                      << candidate.height << " ordered_in=" << candidate.ordered_in << " connection=0x" << std::hex << candidate.connection
                      << std::dec << "\n";
        }

        ASSERT_NE(target, 0u) << "the probe reported a window but the server holds none on screen";

        const auto* window = emu->ui.server.find_window(target);
        ASSERT_NE(window, nullptr);

        // The probe's button, in the CG space the record is written in. inputprobe.m puts a 100x32
        // button at AppKit content-view origin (110, 100) inside a 320x232 content view, and the frame
        // is 264 tall, so the title bar is the 32 pixels above the content: y = 32 + (232 - 116).
        constexpr int32_t button_centre_x = 160;
        constexpr int32_t button_centre_y = 148;

        const auto centre_x = window->x + button_centre_x;
        const auto centre_y = window->y + button_centre_y;
        std::cout << "injecting at (" << centre_x << "," << centre_y << ")\n";

        EXPECT_TRUE(sogen::macos_post_input_event(
            *emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::mouse_moved, .screen_x = centre_x, .screen_y = centre_y}));
        EXPECT_TRUE(sogen::macos_post_input_event(
            *emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_down, .screen_x = centre_x, .screen_y = centre_y}));
        EXPECT_TRUE(sogen::macos_post_input_event(
            *emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::left_mouse_up, .screen_x = centre_x, .screen_y = centre_y}));

        EXPECT_TRUE(sogen::macos_post_input_event(
            *emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::key_down, .keycode = 5, .character = 'g'}));
        EXPECT_TRUE(sogen::macos_post_input_event(
            *emu, sogen::macos_input_event{.kind = sogen::macos_event_kind::key_up, .keycode = 5, .character = 'g'}));

        run(60, "INPUTPROBE-SENDEVENT");

        std::cout << out << std::endl;
        EXPECT_NE(out.find("INPUTPROBE-SENDEVENT"), std::string::npos) << "AppKit never dispatched the injected event";
    }

    // The only native harness that can click. The analyzer front end composes through
    // screenshot_ui_backend, whose input source is off, so an idle GUI app is a finished run there and a
    // click has nowhere to come from; turning it on is what the browser does, and it makes a real
    // application's redraw observable as pixels instead of as a browser run. Disabled because it boots
    // against the host's shared cache and takes minutes.
    //
    //   SOGEN_MACOS_ROOT=/tmp/sogen-macos-root-full SOGEN_CLICK_APP=/System/Applications/Calculator.app \
    //   SOGEN_CLICK_POINTS=33,210;86,210 SOGEN_CLICK_SHOTS=/tmp/click \
    //   SOGEN_CLICK_SPLIT=1 SOGEN_CLICK_FORCE_PRESENT=1 \
    //     macos-emulator-test --gtest_also_run_disabled_tests \
    //     --gtest_filter='EventStream.DISABLED_AClickIsRedrawnIntoTheNextFrame'
    TEST(EventStream, DISABLED_AClickIsRedrawnIntoTheNextFrame)
    {
        const auto root = environment_value("SOGEN_MACOS_ROOT");
        const auto app = environment_value("SOGEN_CLICK_APP");

        if (root.empty() || app.empty() || !std::filesystem::exists(app))
        {
            GTEST_SKIP() << "set SOGEN_MACOS_ROOT to an emulation root and SOGEN_CLICK_APP to the host path of a GUI application";
        }

        const auto launch = sogen::resolve_macos_launch_target(std::filesystem::path{app});
        ASSERT_TRUE(launch.runnable()) << launch.diagnostic;

        constexpr int desktop_width = 640;
        constexpr int desktop_height = 900;

        // Calculator's 7 then its 8, in desktop coordinates.
        std::vector<std::pair<int32_t, int32_t>> points{
            {33, 210},
            {86, 210},
        };

        if (const auto list = environment_value("SOGEN_CLICK_POINTS"); !list.empty())
        {
            points.clear();
            for (size_t start = 0; start <= list.size();)
            {
                const auto end = std::min(list.find(';', start), list.size());
                const auto entry = list.substr(start, end - start);
                start = end + 1;

                if (entry.empty())
                {
                    continue;
                }

                const auto comma = entry.find(',');
                ASSERT_NE(comma, std::string::npos) << "SOGEN_CLICK_POINTS is X,Y;X,Y;... in desktop coordinates";
                points.emplace_back(static_cast<int32_t>(std::strtol(entry.substr(0, comma).c_str(), nullptr, 10)),
                                    static_cast<int32_t>(std::strtol(entry.substr(comma + 1).c_str(), nullptr, 10)));
            }

            ASSERT_FALSE(points.empty()) << "SOGEN_CLICK_POINTS named no point";
        }

        // A batched click posts the move, the press and the release together, which is what the browser's
        // pointer handler does. Splitting them runs the guest to idle in between, so a tracking loop that
        // only starts on the press still sees the release arrive while it is looping.
        const auto split = !environment_value("SOGEN_CLICK_SPLIT").empty();

        // Composites sogen's own layer tree after each click, on top of whatever the guest presented.
        // That is not a frame the guest asked for, so it is off by default; it is the measurement that
        // separates a frame the guest never committed from a tree the guest never changed.
        const auto force_present = !environment_value("SOGEN_CLICK_FORCE_PRESENT").empty();

        const auto shots = environment_value("SOGEN_CLICK_SHOTS");

        auto emu = std::make_unique<sogen::macos_emulator>(macos_test::make_backend(), std::filesystem::path{root});

        auto backend = sogen::create_screenshot_ui_backend();
        auto* const screen = static_cast<sogen::screenshot_ui_backend*>(backend.get());
        screen->set_desktop_size(desktop_width, desktop_height);
        screen->set_input_source(true);
        emu->set_ui_backend(std::move(backend));

        emu->ui.enabled = true;
        emu->ui.desktop_width = desktop_width;
        emu->ui.desktop_height = desktop_height;
        emu->log.disable_output(environment_value("SOGEN_CLICK_VERBOSE").empty());

        sogen::apply_macos_launch_target(launch, emu->file_sys);
        emu->process.current_working_directory = launch.working_directory;

        const std::vector<std::string> envp{"PATH=/usr/bin:/bin", "HOME=/var/root", "TMPDIR=/tmp"};
        ASSERT_TRUE(emu->load_executable(launch.guest_executable, {launch.guest_executable}, envp));

        // A guest with an input source attached parks in the host instead of halting, so a run that has
        // reached its idle loop only comes back through here.
        size_t idle = 0;
        emu->on_host_idle = [&] {
            if (++idle >= 2)
            {
                emu->stop();
            }
        };

        const auto step = [&] {
            idle = 0;
            emu->start(200'000'000);
            const auto reason = emu->last_stop_reason();
            return reason == sogen::stop_reason::none || reason == sogen::stop_reason::instruction_limit ||
                   reason == sogen::stop_reason::explicit_stop;
        };

        size_t presents_before = 0;
        for (size_t i = 0; i < 400 && !emu->process.exit_status.has_value(); ++i)
        {
            if (!step())
            {
                break;
            }

            // A window that has stopped growing its present count is a finished first frame.
            if (screen->present_count() >= 4 && screen->present_count() == presents_before && idle >= 2)
            {
                break;
            }

            presents_before = screen->present_count();
        }

        ASSERT_GT(screen->present_count(), 0u) << "the guest never presented a first frame: " << emu->last_stop_detail();
        const auto before = screen->compose();
        if (!shots.empty())
        {
            EXPECT_TRUE(screen->write(shots + "-before.png"));
        }

        // Held by id rather than by pointer: the window server keeps its windows in a vector, and a
        // window opened between two clicks would leave a cached pointer dangling.
        const auto on_screen = [&]() -> const sogen::macos_window* {
            for (const auto& candidate : emu->ui.server.windows())
            {
                if (candidate.ordered_in && candidate.width > 1 && candidate.height > 1)
                {
                    return &candidate;
                }
            }

            return nullptr;
        };

        ASSERT_NE(on_screen(), nullptr) << "the guest presented without a window on screen";
        const auto window_id = on_screen()->id;

        const auto settle = [&](const size_t presents) {
            for (size_t i = 0; i < 200 && !emu->process.exit_status.has_value(); ++i)
            {
                if (!step())
                {
                    break;
                }

                if (screen->present_count() > presents && idle >= 2)
                {
                    break;
                }
            }
        };

        const auto click = [&](const int32_t x, const int32_t y, const std::string& name) {
            const auto* window = emu->ui.server.find_window(window_id);
            ASSERT_NE(window, nullptr) << "the window the first frame presented is gone";

            const auto point = pack_client_point(static_cast<int16_t>(x - window->x), static_cast<int16_t>(y - window->y));
            std::cout << "clicking " << name << " at desktop (" << x << "," << y << ") in window " << window->id << " " << window->width
                      << "x" << window->height << " at (" << window->x << "," << window->y << ")\n";

            const auto delivered = emu->ui.delivered_input_count();
            const auto presents = screen->present_count();

            // The button state is the one the browser derives from PointerEvent.buttons, and it decides
            // the kind: a move with the button already held is a drag, not a move, and posting one before
            // the press makes the sequence something no pointer ever produces.
            const auto post = [&](const int message, const uint64_t buttons) {
                screen->post_event(sogen::ui_event{
                    .window = window_id,
                    .message = static_cast<uint32_t>(message),
                    .wParam = buttons,
                    .lParam = point,
                });
            };

            post(WM_MOUSEMOVE, 0);
            post(WM_LBUTTONDOWN, MK_LBUTTON);

            if (split)
            {
                settle(presents);
            }

            post(WM_LBUTTONUP, 0);
            settle(presents);

            EXPECT_GT(emu->ui.delivered_input_count(), delivered) << name << " never reached the guest";
            std::cout << name << ": presents " << presents << " -> " << screen->present_count() << "\n";
        };

        const auto changed_pixels = [](const sogen::screenshot_image& a, const sogen::screenshot_image& b) {
            size_t differing = 0;
            for (size_t i = 0; i + 3 < a.rgba.size() && i + 3 < b.rgba.size(); i += 4)
            {
                differing += std::memcmp(&a.rgba[i], &b.rgba[i], 4) == 0 ? 0u : 1u;
            }

            return differing;
        };

        // Only the first key is asserted on. Every later one is reported, because what they do is the
        // defect this harness exists to hold rather than one it guards. Measured on Calculator: the
        // second click reaches the guest and AppKit acts on it -- the display layer takes a new contents
        // object and the transaction goes to the CoreAnimation render server as MIG 40002 -- but nothing
        // commits a SkyLight transaction, and _SLSTransactionCommit is the only thing that makes sogen
        // composite the tree or rasterise a layer's contents. With SOGEN_CLICK_FORCE_PRESENT the tree
        // composites 160 pixels away from the frame on screen, and the new contents counts as
        // unresolved, so the digit the first click drew disappears instead of the second one arriving.
        auto previous = before;
        for (size_t index = 0; index < points.size(); ++index)
        {
            const auto name = "key " + std::to_string(index + 1);
            click(points[index].first, points[index].second, name);

            const auto after = screen->compose();
            if (!shots.empty())
            {
                EXPECT_TRUE(screen->write(shots + "-after" + std::to_string(index + 1) + ".png"));
            }

            ASSERT_EQ(previous.rgba.size(), after.rgba.size());
            const auto differing = changed_pixels(previous, after);
            std::cout << differing << " pixels changed by " << name << "\n";

            auto latest = after;
            if (force_present)
            {
                const auto windows = sogen::macos_layer_tree_present(*emu);
                latest = screen->compose();
                std::cout << "compositing the tree after " << name << " presented " << windows << " window(s) and moved "
                          << changed_pixels(after, latest) << " pixels\n";
            }

            if (index == 0)
            {
                EXPECT_GT(differing, 0u) << "the frame the first click produced is identical to the one before it";
            }

            previous = latest;
        }
    }

    TEST(EventStream, AUiEventForAnUnknownWindowIsReportedByName)
    {
        translate_fixture fixture{};
        fixture.prepare_window(0, 0, 320, 232);

        std::string captured{};
        fixture.emu->log.set_sink([&](sogen::color, const std::string_view message) { captured.append(message); });

        EXPECT_FALSE(sogen::macos_translate_ui_event(*fixture.emu,
                                                     sogen::ui_event{.window = 0xDEAD, .message = WM_LBUTTONDOWN, .wParam = MK_LBUTTON}));

        EXPECT_NE(captured.find("window server does not own"), std::string::npos) << captured;
    }
}
