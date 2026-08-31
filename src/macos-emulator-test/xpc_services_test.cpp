#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/xpc_bootstrap.hpp>
#include <mach/xpc_services.hpp>
#include <mach/mach_types.hpp>

namespace
{
    using namespace sogen::xpc; // NOLINT(google-build-using-namespace)

    // The policy is name-independent: a daemon sogen does not run is still reachable, and answers
    // every message with a refusal. Nothing here may grow into a list of the services one app happens
    // to look up.
    TEST(XpcServices, EveryServiceNameGetsALivePortWhoseMessagesAreRefused)
    {
        for (const auto* name : {"com.apple.tccd", "com.apple.cfprefsd.daemon", "com.apple.system.opendirectoryd.libinfo",
                                 "com.apple.pasteboard.1", "com.apple.coreservices.launchservices.session.7", "com.example.invented"})
        {
            const auto decision = decide_xpc_service(name);
            ASSERT_TRUE(decision.has_value()) << name;
            EXPECT_EQ(*decision, xpc_service_decision::refuse_connection_invalid) << name;
        }
    }

    TEST(XpcServices, AnEmptyNameIsNotAService)
    {
        EXPECT_FALSE(decide_xpc_service("").has_value());
    }

    TEST(XpcServices, LogdGetsNoSuchServiceRatherThanARefusedPort)
    {
        // Measured 2026-08-27 on the host: libtrace completes os_log cleanly when the logd lookup fails
        // (verified under lldb by failing bootstrap_look_up3 natively), while a live-but-dead logd port
        // crashes the firehose push path with MIG_REPLY_MISMATCH.
        EXPECT_FALSE(decide_xpc_service("com.apple.logd").has_value());
    }

    // The window server is the one name sogen serves for real, so a failed lookup is no longer the
    // truth about it. Measured 2026-08-28 with src/tools/macos-gui-probe/appkitwin: HIToolbox's
    // _CheckEventsInited calls CGWindowServerCFMachPort, which is CGSLookupServerPort (a lookup of this
    // name, then MIG 29010 GetSessionPort on what comes back) wrapped in CFMachPortCreateWithPort. With
    // the lookup failing, that returns NULL, _CheckEventsInited bails before INIT_AppleEvents, the
    // kAEOpenApplication event HIToolbox synthesises there is never created, and AppKit never posts
    // NSApplicationDidFinishLaunchingNotification -- so the app never makes a window.
    TEST(XpcServices, TheWindowServerLookupProducesSogensOwnServerPort)
    {
        const auto decision = decide_xpc_service("com.apple.windowserver.active");
        ASSERT_TRUE(decision.has_value());
        EXPECT_EQ(*decision, xpc_service_decision::window_server);
    }

    TEST(XpcServices, AnEndpointValueMatchesTheMeasuredWireShape)
    {
        const auto bytes = serialize(make_dictionary({{"port", make_endpoint(0)}}));

        ASSERT_EQ(bytes.size(), 32u);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 8), type::dictionary);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 16), 1u);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 28), type::endpoint)
            << "measured 2026-08-27: launchd's lookup reply tags the port entry 0xd000";
        EXPECT_EQ(sogen::mach::read_u32(bytes, 12), 0x10u) << "the tag carries no payload; the right itself travels as a descriptor";

        const auto parsed = parse(bytes);
        ASSERT_TRUE(parsed.has_value());
        ASSERT_NE(find(*parsed, "port"), nullptr);
        EXPECT_EQ(find(*parsed, "port")->type_tag, type::endpoint);
    }

    TEST(XpcServices, AnMqPostDeliversTheConnectionPingToTheService)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.tccd")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto service =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE)).name;

        constexpr uint64_t base = 0x350000000ULL;
        emu->memory.allocate_memory(base, 4 * sogen::MACOS_PAGE_SIZE, sogen::memory_permission::all);

        const uint64_t w00t_address = base;
        const uint64_t ping_address = base + sogen::MACOS_PAGE_SIZE;
        const uint64_t dmm_address = base + 2 * sogen::MACOS_PAGE_SIZE;
        const uint64_t code_page = base + 3 * sogen::MACOS_PAGE_SIZE;

        const auto channel = emu->mach.ports.allocate_receive_right();
        const auto reply_port = emu->mach.make_special_reply_port(1);

        // The check-in a connection posts through the mq form (measured 2026-08-27): the inner message
        // carries the channel port's receive right, nothing else.
        std::vector<uint8_t> w00t(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE, 0);
        sogen::mach::write_msg_header(w00t,
                                      {.bits = sogen::mach::BITS_COMPLEX | sogen::mach::make_bits(sogen::mach::disposition::copy_send, 0),
                                       .size = static_cast<uint32_t>(w00t.size()),
                                       .remote_port = service,
                                       .local_port = 0,
                                       .voucher_port = 0,
                                       .id = 0x77303074});
        sogen::mach::write_u32(w00t, sogen::mach::MSG_HEADER_SIZE, 1);
        sogen::mach::write_port_descriptor(
            std::span{w00t}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE),
            {.name = channel, .disposition = sogen::mach::disposition::move_receive, .type = sogen::mach::descriptor_type::port});

        const auto ping_body = serialize(make_dictionary({{"probe", make_string("x")}}));
        std::vector<uint8_t> ping(sogen::mach::MSG_HEADER_SIZE, 0);
        sogen::mach::write_msg_header(
            ping, {.bits = sogen::mach::make_bits(sogen::mach::disposition::copy_send, sogen::mach::disposition::make_send_once),
                   .size = 0,
                   .remote_port = channel,
                   .local_port = reply_port,
                   .voucher_port = 0,
                   .id = 0x40000000});
        ping.insert(ping.end(), ping_body.begin(), ping_body.end());
        sogen::mach::write_u32(ping, 4, static_cast<uint32_t>(ping.size()));

        emu->memory.write_memory(w00t_address, w00t.data(), w00t.size());
        emu->memory.write_memory(ping_address, ping.data(), ping.size());

        // The vector form libdispatch posts through: the message travels as element 0 of a
        // mach_msg_vector_t array, the trap's size halves carry the element count, and the header fields
        // in the trap registers duplicate the message's own header.
        uint64_t next_code = code_page;
        const auto post = [&](const uint64_t message_address, const std::vector<uint8_t>& message, const uint32_t descriptors) {
            std::array<uint8_t, sogen::mach::MSG_VECTOR_ELEMENT_SIZE> element{};
            sogen::mach::write_msg_vector_element(element, {.data = message_address, .send_size = static_cast<uint32_t>(message.size())});
            emu->memory.write_memory(dmm_address, element.data(), element.size());

            const auto header = sogen::mach::read_msg_header(message);

            macos_test::mach_msg2_args args{};
            args.buffer = dmm_address;
            args.options = sogen::mach::msg_option::send_msg | sogen::mach::msg_option::mq_call | sogen::mach::msg_option::vector;
            args.send_size = 1;
            args.bits = header.bits;
            args.remote_port = header.remote_port;
            args.local_port = header.local_port;
            args.voucher_port = header.voucher_port;
            args.id = static_cast<uint32_t>(header.id);
            args.descriptor_count = descriptors;

            const auto words = macos_test::mach_msg2_words(args);

            // Each post gets its own address: the backend caches its translation of a code page, so a
            // second program written over the first one runs the first one again.
            macos_test::write_guest_code(*emu, next_code, words);
            emu->start(words.size());
            next_code += 0x100;
        };

        post(w00t_address, w00t, 1);
        EXPECT_EQ(emu->mach.ports.object_of(channel).kind, sogen::mach::kernel_object_kind::xpc_service)
            << "the check-in hands the channel port to the daemon";

        // Measured 2026-08-27 on the host (cgsdemo under lldb): the daemon's replies arrive on the
        // connection's channel port, which is what the client monitors with an EVFILT_MACHPORT knote
        // and drains through the mq receive -- the message's reply port is not where channel traffic
        // lands.
        auto& workq = emu->process.kqueues.ensure(sogen::MACOS_PROCESS_WORKQ_ID);
        workq.registrations.push_back(
            {.filter = sogen::MACOS_EVFILT_MACHPORT, .ident = channel, .flags = 0x385, .fflags = 0x0700080e, .udata = 0xCAFE});

        post(ping_address, ping, 0);

        const auto* port = emu->mach.ports.destination_of(channel);
        ASSERT_NE(port, nullptr);
        ASSERT_EQ(port->queue.size(), 1u) << "the ping's refusal reply arrives on the connection's channel port";

        const auto& queued = port->queue.front();
        const auto header = sogen::mach::read_msg_header(queued);
        EXPECT_EQ(header.id, 0x20000000);
        EXPECT_EQ(header.local_port, reply_port) << "the header still names the port the reply was addressed to";
        const auto parsed = parse(std::span{queued}.subspan(sogen::mach::MSG_HEADER_SIZE, header.size - sogen::mach::MSG_HEADER_SIZE));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_TRUE(parsed->entries.empty());

        ASSERT_EQ(emu->process.kqueues.pending_count(sogen::MACOS_PROCESS_WORKQ_ID), 1u)
            << "the reply on the monitored channel port fires the knote";
        sogen::kevent_registration events[1]{};
        ASSERT_EQ(emu->process.kqueues.deliver(sogen::MACOS_PROCESS_WORKQ_ID, events, 1), 1u);
        EXPECT_EQ(events[0].ident, uint64_t{channel});
        EXPECT_EQ(events[0].filter, sogen::MACOS_EVFILT_MACHPORT);
        EXPECT_EQ(events[0].udata, 0xCAFEu);
    }

    TEST(XpcServices, TheRefusalReplyIsTheMeasuredEmptyDictionary)
    {
        // Measured 2026-08-27: tccd answers a message it does not understand with an empty dictionary.
        const auto parsed = parse(refused_service_reply_body());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->type_tag, type::dictionary);
        EXPECT_TRUE(parsed->entries.empty());
    }

    TEST(XpcServices, ANotifyCheckinGetsTheMeasuredNotifydReply)
    {
        const auto emu = macos_test::make_emulator();
        const auto service = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::xpc_service});

        const auto reply = macos_test::send_mig_call(*emu, service, 0x3ff, {}, 64);
        const auto header = sogen::mach::read_msg_header(reply);

        // Measured 2026-08-27 (cgsdemo under lldb): notifyd's checkin reply is 48 bytes, id+100, NDR,
        // then a zero word, version 3 (notify_client.c rejects anything below 3), the server pid, and
        // status OK.
        EXPECT_EQ(header.size, 0x30u);
        EXPECT_EQ(header.id, 0x3ff + 100);
        EXPECT_EQ(sogen::mach::read_u32(reply, 32), 0u);
        EXPECT_EQ(sogen::mach::read_u32(reply, 36), 3u) << "version";
        EXPECT_EQ(sogen::mach::read_u32(reply, 44), 0u) << "status OK";
    }

    TEST(XpcServices, ANotifyGenerateCommonPortHandsOverARealPort)
    {
        const auto emu = macos_test::make_emulator();
        const auto service = emu->mach.ports.allocate_receive_right({.kind = sogen::mach::kernel_object_kind::xpc_service});

        const auto reply = macos_test::send_mig_call(*emu, service, 0x401, {}, 96);
        const auto header = sogen::mach::read_msg_header(reply);

        // Measured 2026-08-27: complex 52-byte reply, one move-receive port descriptor, status OK.
        EXPECT_EQ(header.size, 0x34u);
        EXPECT_EQ(header.id, 0x401 + 100);
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u);

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.disposition, sogen::mach::disposition::move_receive);
        EXPECT_TRUE(emu->mach.ports.exists(descriptor.name)) << "the common port is a real port";
    }
}
