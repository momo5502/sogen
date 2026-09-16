#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <gui/macos_window_server_mig.hpp>
#include <mach/xpc_bootstrap.hpp>
#include <mach/mach_types.hpp>

namespace
{
    using namespace sogen::xpc; // NOLINT(google-build-using-namespace)

    TEST(XpcCodec, TwoEntryDictionaryMatchesTheObservedByteImage)
    {
        const auto bytes = serialize(make_dictionary({
            {"handle", make_uint64(0x2e74)},
            {"type", make_uint64(5)},
        }));

        ASSERT_EQ(bytes.size(), 60u) << "8 magic+version, 12 dictionary header, 20 per uint64 entry";
        EXPECT_EQ(sogen::mach::read_u32(bytes, 0), MAGIC);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 4), VERSION);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 8), type::dictionary);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 12), 0x2cu) << "size counts from the count field onward";
        EXPECT_EQ(sogen::mach::read_u32(bytes, 16), 2u);

        const std::array<uint8_t, 8> key{'h', 'a', 'n', 'd', 'l', 'e', 0, 0};
        EXPECT_TRUE(std::equal(key.begin(), key.end(), bytes.begin() + 20)) << "keys are NUL-padded to 4 bytes";
        EXPECT_EQ(sogen::mach::read_u32(bytes, 28), type::uint64);
        EXPECT_EQ(sogen::mach::read_u64(bytes, 32), 0x2e74u);

        const std::array<uint8_t, 8> key2{'t', 'y', 'p', 'e', 0, 0, 0, 0};
        EXPECT_TRUE(std::equal(key2.begin(), key2.end(), bytes.begin() + 40));
        EXPECT_EQ(sogen::mach::read_u64(bytes, 52), 5u);
    }

    TEST(XpcCodec, FiveEntryRequestReproducesTheObservedTotalSize)
    {
        const auto bytes = serialize(make_dictionary({
            {"handle", make_uint64(0x2e74)},
            {"environment", make_dictionary({})},
            {"paths", make_dictionary({})},
            {"origin", make_string(std::string(132, 'a'))},
            {"type", make_uint64(5)},
        }));

        EXPECT_EQ(sogen::mach::MSG_HEADER_SIZE + bytes.size(), 280u) << "the measured send size of the first launchd message";
        EXPECT_EQ(sogen::mach::read_u32(bytes, 12), 0xf0u);
        EXPECT_EQ(sogen::mach::read_u32(bytes, 16), 5u);
    }

    TEST(XpcCodec, RoundTripsEveryValueKind)
    {
        const auto original = make_dictionary({
            {"handle", make_uint64(0x2e74)},
            {"origin", make_string("/tmp")},
            {"empty", make_dictionary({})},
        });

        const auto parsed = parse(serialize(original));
        ASSERT_TRUE(parsed.has_value());
        ASSERT_NE(find(*parsed, "handle"), nullptr);
        EXPECT_EQ(find(*parsed, "handle")->number, 0x2e74u);
        ASSERT_NE(find(*parsed, "origin"), nullptr);
        EXPECT_EQ(find(*parsed, "origin")->text, "/tmp");
        ASSERT_NE(find(*parsed, "empty"), nullptr);
        EXPECT_TRUE(find(*parsed, "empty")->entries.empty());
        EXPECT_EQ(find(*parsed, "missing"), nullptr);
    }

    TEST(XpcCodec, RejectsAForeignMagicAndSurvivesTruncation)
    {
        std::vector<uint8_t> bad(16, 0);
        sogen::mach::write_u32(bad, 0, 0xDEADBEEF);
        EXPECT_FALSE(parse(bad).has_value());

        auto truncated = serialize(make_dictionary({{"handle", make_uint64(1)}, {"type", make_uint64(5)}}));
        truncated.resize(20);
        EXPECT_NO_THROW((void)parse(truncated)) << "the third observed launchd send is exactly this shape";
    }

    TEST(XpcResponder, AnswersTheLaunchdRoutinesWithTheSameId)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({{"handle", make_uint64(0x2e74)}, {"type", make_uint64(5)}}));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000323, request, 96);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(static_cast<uint32_t>(header.id), 0x40000323u) << "XPC replies carry the same id, never id + 100";
        EXPECT_EQ(header.size, 84u);
        EXPECT_EQ(header.remote_port, sogen::mach::PORT_NULL) << "a reply conveys no right back";
        EXPECT_EQ(header.local_port, emu->mach.make_special_reply_port(1)) << "it arrived on the reply port";

        const auto parsed = parse(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE, header.size - sogen::mach::MSG_HEADER_SIZE));
        ASSERT_TRUE(parsed.has_value());
        ASSERT_NE(find(*parsed, "handle"), nullptr);
        EXPECT_EQ(find(*parsed, "handle")->number, 0x2e74u);
        ASSERT_NE(find(*parsed, "type"), nullptr);
        EXPECT_EQ(find(*parsed, "type")->number, 5u);
    }

    TEST(XpcResponder, AnUnknownRoutineOnTheBootstrapPortIsNotAnswered)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({{"handle", make_uint64(1)}}));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000999, request, 96);

        EXPECT_EQ(sogen::mach::read_msg_header(reply).size, sogen::mach::MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(sogen::mach::read_u32(reply, 32)), sogen::mach::mig_error::bad_id);
    }

    TEST(XpcResponder, AServiceLookupReturnsALiveServicePort)
    {
        const auto emu = macos_test::make_emulator();

        // A lookup without a "lookup-handle" is the second round libxpc sends, the one launchd answers
        // with the service port itself (measured 2026-08-27).
        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.cfprefsd.daemon")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(header.id, 0x20000000) << "measured: launchd's lookup result carries this id, not the request id";
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u) << "the port right travels as a descriptor";

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.type, sogen::mach::descriptor_type::port);
        EXPECT_EQ(descriptor.disposition, sogen::mach::disposition::move_send);
        EXPECT_NE(descriptor.name, 0u);
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, sogen::mach::kernel_object_kind::xpc_service);

        const auto dict_offset = sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE;
        const auto parsed = parse(std::span{reply}.subspan(dict_offset, header.size - dict_offset));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_NE(find(*parsed, "rec_execcnt"), nullptr);
        ASSERT_NE(find(*parsed, "req_pid"), nullptr);
        EXPECT_EQ(find(*parsed, "req_pid")->number, 4242u) << "the requester's own pid, as the guest knows it";
        ASSERT_NE(find(*parsed, "port"), nullptr);
        EXPECT_EQ(find(*parsed, "port")->type_tag, type::endpoint);
    }

    TEST(XpcResponder, AMessageToAServicePortGetsTheRefusalReply)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.tccd")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));

        const auto message = serialize(make_dictionary({{"probe", make_string("hello")}}));
        const auto service_reply = macos_test::send_mig_call(*emu, descriptor.name, 0x40000000, message, 256);
        const auto header = sogen::mach::read_msg_header(service_reply);

        EXPECT_EQ(header.id, 0x20000000) << "measured: a daemon's reply carries this id, not the request id";

        const auto parsed =
            parse(std::span{service_reply}.subspan(sogen::mach::MSG_HEADER_SIZE, header.size - sogen::mach::MSG_HEADER_SIZE));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->type_tag, type::dictionary);
        EXPECT_TRUE(parsed->entries.empty()) << "the measured tccd reply to a message it does not understand";
    }

    // The check-in is the one message on a service port that is send-only: it hands the daemon the
    // connection's channel right and expects nothing back. Answering it left a reply queued on the
    // service port for every connection the guest ever opened, and nothing was ever going to receive
    // one -- a real client only receives on the channel.
    TEST(XpcResponder, AConnectionCheckInIsAdoptedAndAnsweredWithNothing)
    {
        const auto emu = macos_test::make_emulator();

        const auto lookup = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.tccd")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, lookup, 256);
        const auto service =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE)).name;

        const auto* before = emu->mach.ports.find(service);
        ASSERT_NE(before, nullptr);
        const auto queued_before = before->queue.size();

        const auto channel = emu->mach.ports.allocate_receive_right();

        std::vector<uint8_t> checkin(sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE, 0);
        sogen::mach::write_u32(checkin, 0, 1);
        sogen::mach::write_port_descriptor(
            std::span{checkin}.subspan(sogen::mach::MSG_BODY_SIZE),
            {.name = channel, .disposition = sogen::mach::disposition::move_receive, .type = sogen::mach::descriptor_type::port});

        macos_test::send_mig_call(*emu, service, 0x77303074, checkin, 256, true);

        EXPECT_EQ(emu->mach.ports.object_of(channel).kind, sogen::mach::kernel_object_kind::xpc_service)
            << "the channel the check-in carried is the daemon's to answer from now on";

        const auto* after = emu->mach.ports.find(service);
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(after->queue.size(), queued_before) << "a send-only check-in leaves nothing queued behind it";
    }

    TEST(XpcResponder, AFullLookupWithALookupHandleAlsoReturnsTheServicePort)
    {
        const auto emu = macos_test::make_emulator();

        // The measured opendirectoryd lookup shape: five entries, lookup-handle included. launchd
        // answers it with the port directly -- there is no second round.
        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(8)},
            {"name", make_string("com.apple.system.opendirectoryd.libinfo")},
            {"type", make_uint64(1)},
            {"lookup-handle", make_uint64(0)},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(header.id, 0x20000000);
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u);

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.type, sogen::mach::descriptor_type::port);
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, sogen::mach::kernel_object_kind::xpc_service);
    }

    TEST(XpcResponder, AByNameLookupRoutineReturnsTheServicePort)
    {
        const auto emu = macos_test::make_emulator();

        // Routine 0x400000cf is the lookup variant bootstrap_look_up3 uses: a complex message whose
        // descriptor is the bootstrap port, followed by the same flat dictionary.
        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(8)},
            {"name", make_string("com.apple.system.notification_center")},
            {"type", make_uint64(7)},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x400000cf, request, 256);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(header.id, 0x20000000);
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u);

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(descriptor.type, sogen::mach::descriptor_type::port);
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, sogen::mach::kernel_object_kind::xpc_service);
    }

    TEST(XpcResponder, AMigMessageToAServicePortGetsBadId)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.system.notification_center")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));

        // notifyd's IPC is plain MIG on the notification-center port. The checkin/register/check family
        // is answered with the measured replies; a routine outside it (here: 1030, past the last one
        // notify.c sends) gets the same named bad-id any other unimplemented port kind gives.
        const auto mig_reply = macos_test::send_mig_call(*emu, descriptor.name, 1030, macos_test::ndr_body({0}), 96);
        EXPECT_EQ(sogen::mach::read_msg_header(mig_reply).size, sogen::mach::MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(static_cast<int32_t>(sogen::mach::read_u32(mig_reply, 32)), sogen::mach::mig_error::bad_id);
    }

    // notify.c only maps notifyd's shared segment when the registration names a slot in it, and sogen
    // has no POSIX shared memory namespace for shm_open("apple.shm.notification_center") to find. The
    // registration therefore reports no slot, which sends every later notify_check() to routine 1002 on
    // the same port instead of to the segment.
    TEST(XpcResponder, ANotifyRegistrationReportsNoSharedSlotAndAnswersTheCheckItself)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.system.notification_center")},
        }));
        const auto lookup = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{lookup}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));

        const auto registration = macos_test::send_mig_call(*emu, descriptor.name, 1012, macos_test::ndr_body({0}), 96);
        EXPECT_EQ(static_cast<int32_t>(sogen::mach::read_u32(registration, 36)), -1) << "no slot in a segment that does not exist";
        EXPECT_EQ(sogen::mach::read_u32(registration, 52), 0u) << "the registration itself still succeeds";

        const auto check = macos_test::send_mig_call(*emu, descriptor.name, 1002, macos_test::ndr_body({0}), 96);
        EXPECT_NE(sogen::mach::read_msg_header(check).size, sogen::mach::MIG_REPLY_ERROR_SIZE);
        EXPECT_EQ(sogen::mach::read_u32(check, 36), 0u) << "nothing in sogen posts, so the name never changes";
        EXPECT_EQ(sogen::mach::read_u32(check, 40), 0u);
    }

    // A daemon sogen does not run is still reachable: the lookup hands out a port whose every message
    // is refused. The failed-lookup shape is reserved for the two names measured to need it, because
    // libxpc leaves a failed connection with no send right and a synchronous send on one waits forever
    // for a reply nothing will generate.
    TEST(XpcResponder, AServiceNameSogenHasNoDaemonForStillGetsAPort)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.example.no.such.service")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x40000324, request, 256);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(header.id, 0x20000000);
        EXPECT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u) << "the reply carries the service port as a descriptor";

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, sogen::mach::kernel_object_kind::xpc_service);

        const auto dict_offset = sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE;
        const auto parsed = parse(std::span{reply}.subspan(dict_offset, header.size - dict_offset));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(find(*parsed, "error"), nullptr);
        ASSERT_NE(find(*parsed, "port"), nullptr);
    }

    TEST(XpcResponder, TheLogdLookupFailsLikeANonexistentService)
    {
        // Measured 2026-08-27 on the host: launchd's no-such-service answer is the one refusal shape
        // libtrace treats as terminal -- os_log runs its no-logd degradation instead of dying in the
        // firehose push. sogen runs no logd, so the failed lookup is the truth.
        for (const auto* name : {"com.apple.logd"})
        {
            const auto emu = macos_test::make_emulator();

            const auto request = serialize(make_dictionary({
                {"handle", make_uint64(0)},
                {"flags", make_uint64(0)},
                {"name", make_string(name)},
            }));
            const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x400000cf, request, 256);
            const auto header = sogen::mach::read_msg_header(reply);

            EXPECT_EQ(header.id, 0x20000000) << name;
            EXPECT_EQ(header.bits & sogen::mach::BITS_COMPLEX, 0u) << name << ": a failed lookup hands out no port";

            const auto parsed = parse(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE, header.size - sogen::mach::MSG_HEADER_SIZE));
            ASSERT_TRUE(parsed.has_value()) << name;
            ASSERT_NE(find(*parsed, "error"), nullptr) << name;
            EXPECT_EQ(find(*parsed, "error")->number, 3u) << name;
            EXPECT_EQ(find(*parsed, "port"), nullptr) << name;
        }
    }

    // The window server is the exception: sogen answers its routines, so the lookup hands out the very
    // port gui/macos_window_server_mig.cpp serves rather than a stand-in. Measured 2026-08-28 -- with a
    // failed lookup, CGSLookupServerPort returns 0, CGWindowServerCFMachPort returns NULL, and
    // HIToolbox's _CheckEventsInited bails before INIT_AppleEvents synthesises kAEOpenApplication.
    TEST(XpcResponder, TheWindowServerLookupHandsOutTheWindowServerPort)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = serialize(make_dictionary({
            {"handle", make_uint64(0)},
            {"flags", make_uint64(0)},
            {"name", make_string("com.apple.windowserver.active")},
        }));
        const auto reply = macos_test::send_mig_call(*emu, emu->mach.bootstrap, 0x400000cf, request, 256);
        const auto header = sogen::mach::read_msg_header(reply);

        EXPECT_EQ(header.id, 0x20000000);
        ASSERT_NE(header.bits & sogen::mach::BITS_COMPLEX, 0u) << "the reply carries the window-server port as a descriptor";

        const auto descriptor =
            sogen::mach::read_port_descriptor(std::span{reply}.subspan(sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE));
        EXPECT_EQ(emu->mach.ports.object_of(descriptor.name).kind, sogen::mach::kernel_object_kind::window_server);
        EXPECT_EQ(descriptor.name, sogen::macos_window_server_session_port(*emu))
            << "the same port SLSServerPort hands out, so 29010 GetSessionPort answers with itself";

        const auto dict_offset = sogen::mach::MSG_HEADER_SIZE + sogen::mach::MSG_BODY_SIZE + sogen::mach::PORT_DESCRIPTOR_SIZE;
        const auto parsed = parse(std::span{reply}.subspan(dict_offset, header.size - dict_offset));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(find(*parsed, "error"), nullptr);
        ASSERT_NE(find(*parsed, "port"), nullptr);
    }

    // Named, not just refused. An unimplemented routine here was silent, unlike an unimplemented MIG
    // routine, so a guest stuck on a service sogen does not run gave no clue which request it was stuck
    // on. This is what turned a hang into "the look-up is for com.apple.cfprefsd.daemon".
    TEST(XpcResponder, AnXpcRoutineIsRecognisedByItsHighBit)
    {
        EXPECT_TRUE(sogen::xpc::bootstrap_responder::is_xpc_routine(0x40000324));
        EXPECT_TRUE(sogen::xpc::bootstrap_responder::is_xpc_routine(0x40000999));
        EXPECT_FALSE(sogen::xpc::bootstrap_responder::is_xpc_routine(3409)) << "an ordinary MIG routine is not one of these";
        EXPECT_FALSE(sogen::xpc::bootstrap_responder::is_xpc_routine(0));
    }
}
