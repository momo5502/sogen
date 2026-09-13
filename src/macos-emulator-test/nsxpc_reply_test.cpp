#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_types.hpp>
#include <mach/nsxpc_reply.hpp>
#include <mach/xpc_bootstrap.hpp>
#include <mach/xpc_services.hpp>

namespace
{
    using namespace sogen::nsxpc; // NOLINT(google-build-using-namespace)

    // Captured 2026-08-28 with lldb from -[NSXPCConnection _sendInvocation:...] on the host: the "root"
    // entry of the request LaunchServices sends com.apple.lsd.mapdb, and the "root" entry of a reply whose
    // block signature is v@?B. Both are the ground truth for the encoder below; nothing here is inferred.
    constexpr uint8_t MEASURED_REQUEST_ROOT[] = {
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x31, 0x37, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x11,
        0x30, 0x67, 0x65, 0x74, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72, 0x53, 0x74, 0x6f, 0x72, 0x65, 0x4e, 0x6f, 0x6e, 0x42,
        0x6c, 0x6f, 0x63, 0x6b, 0x69, 0x6e, 0x67, 0x57, 0x69, 0x74, 0x68, 0x43, 0x6f, 0x6d, 0x70, 0x6c, 0x65, 0x74, 0x69,
        0x6f, 0x6e, 0x48, 0x61, 0x6e, 0x64, 0x6c, 0x65, 0x72, 0x3a, 0x00, 0x7c, 0x76, 0x32, 0x34, 0x40, 0x30, 0x3a, 0x38,
        0x40, 0x3f, 0x31, 0x36, 0x00, 0xa0, 0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0,
    };

    constexpr uint8_t MEASURED_REPLY_ROOT[] = {
        0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x31, 0x37, 0xa0, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xe0, 0x75, 0x76, 0x40, 0x3f, 0x42, 0x00, 0xa0, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0,
    };

    constexpr std::string_view MAPDB_REPLY_SIGNATURE = "v44@?0@8@\"FSNode\"16@\"NSXPCListenerEndpoint\"24B32@\"NSError\"36";

    TEST(NsxpcReply, TheEmbeddedSignatureIsTheReplysigWithoutItsOffsets)
    {
        EXPECT_EQ(strip_type_encoding_offsets(MAPDB_REPLY_SIGNATURE), "v@?@@\"FSNode\"@\"NSXPCListenerEndpoint\"B@\"NSError\"");
    }

    TEST(NsxpcReply, TheReplyArgumentsExcludeTheBlockItself)
    {
        const auto types = reply_argument_types("v@?@@\"FSNode\"@\"NSXPCListenerEndpoint\"B@\"NSError\"");
        ASSERT_TRUE(types.has_value());
        EXPECT_EQ(*types, (std::vector<std::string>{"@", "@\"FSNode\"", "@\"NSXPCListenerEndpoint\"", "B", "@\"NSError\""}));
    }

    TEST(NsxpcReply, ASignatureThatIsNotAReplyBlocksIsRefused)
    {
        // A reply block always returns void and always takes itself first. Anything else is not one, and
        // guessing an encoding for it would put bytes on the wire that no client asked for.
        EXPECT_FALSE(reply_argument_types("i@?B").has_value()) << "non-void return";
        EXPECT_FALSE(reply_argument_types("v@B").has_value()) << "first argument is not the block";
        EXPECT_FALSE(reply_argument_types("v").has_value()) << "no block at all";
        EXPECT_EQ(reply_argument_types("v@?"), (std::vector<std::string>{})) << "a block taking nothing is still one";
    }

    // The encoder is checked against Foundation's own output, not against itself: this is a real reply
    // Foundation produced, and rebuilding it byte for byte is what says the tags, the lengths and the
    // absolute end offsets are all right.
    TEST(NsxpcReply, TheEncoderReproducesAMeasuredFoundationReply)
    {
        std::string unsupported{};
        const auto root = empty_reply_root("v20@?0B16", &unsupported);
        ASSERT_TRUE(root.has_value()) << unsupported;
        EXPECT_EQ(*root, std::vector<uint8_t>(std::begin(MEASURED_REPLY_ROOT), std::end(MEASURED_REPLY_ROOT)));
    }

    TEST(NsxpcReply, TheMapdbReplyIsFiveNilsAndANo)
    {
        std::string unsupported{};
        const auto root = empty_reply_root(MAPDB_REPLY_SIGNATURE, &unsupported);
        ASSERT_TRUE(root.has_value()) << unsupported;

        // The array of arguments is the tail of the payload: four nils around one false, in signature
        // order (store, FSNode, endpoint, BOOL, NSError).
        const std::vector<uint8_t> arguments{0xe0, 0xe0, 0xe0, 0xb0, 0xe0};
        ASSERT_GE(root->size(), arguments.size());
        EXPECT_TRUE(std::equal(arguments.begin(), arguments.end(), root->end() - static_cast<ptrdiff_t>(arguments.size())));
    }

    TEST(NsxpcReply, AnArgumentTypeWithNoMeasuredEncodingIsRefusedByName)
    {
        std::string unsupported{};
        EXPECT_FALSE(empty_reply_root("v20@?0d8", &unsupported).has_value());
        EXPECT_EQ(unsupported, "d") << "the double is named so it can be implemented rather than guessed";
    }

    TEST(NsxpcReply, TheSelectorIsReadOutOfAMeasuredRequest)
    {
        const std::vector<uint8_t> root(std::begin(MEASURED_REQUEST_ROOT), std::end(MEASURED_REQUEST_ROOT));
        const auto selector = invocation_selector(root);
        ASSERT_TRUE(selector.has_value());
        EXPECT_EQ(*selector, "getServerStoreNonBlockingWithCompletionHandler:");
    }

    TEST(NsxpcReply, AReplyPayloadNamesNoSelector)
    {
        const std::vector<uint8_t> root(std::begin(MEASURED_REPLY_ROOT), std::end(MEASURED_REPLY_ROOT));
        EXPECT_FALSE(invocation_selector(root).has_value());
    }

    // The whole point: an empty dictionary is not something NSXPC can decode, so the client's reply block
    // never runs and it sends again. Measured 2026-08-28 -- LaunchServices sent 281 times in 40e9
    // instructions under sogen, and on the host, fed the same empty dictionary through an
    // xpc_connection_send_message_with_reply_sync interposer, it never stopped either.
    TEST(XpcServices, AnNsxpcInvocationIsAnsweredWithADecodableReplyRatherThanAnEmptyDictionary)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = sogen::xpc::serialize(sogen::xpc::make_dictionary({
            {"f", sogen::xpc::make_uint64(33)},
            {"root", sogen::xpc::make_data({std::begin(MEASURED_REQUEST_ROOT), std::end(MEASURED_REQUEST_ROOT)})},
            {"proxynum", sogen::xpc::make_uint64(1)},
            {"replysig", sogen::xpc::make_string(std::string{MAPDB_REPLY_SIGNATURE})},
            {"sequence", sogen::xpc::make_uint64(1)},
        }));

        const auto body = sogen::xpc::nsxpc_refusal_body(*emu, request);
        ASSERT_TRUE(body.has_value());

        const auto parsed = sogen::xpc::parse(*body);
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->entries.size(), 1u);

        const auto* root = sogen::xpc::find(*parsed, "root");
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(root->type_tag, sogen::xpc::type::data);

        std::string unsupported{};
        EXPECT_EQ(root->bytes, empty_reply_root(MAPDB_REPLY_SIGNATURE, &unsupported));
    }

    TEST(XpcServices, AMessageThatIsNotAnNsxpcInvocationKeepsTheEmptyDictionaryRefusal)
    {
        const auto emu = macos_test::make_emulator();

        const auto request = sogen::xpc::serialize(sogen::xpc::make_dictionary({
            {"rpc_version", {.type_tag = sogen::xpc::type::int64, .number = 2}},
            {"rpc_name", sogen::xpc::make_string("getpwuid")},
        }));

        EXPECT_FALSE(sogen::xpc::nsxpc_refusal_body(*emu, request).has_value());
    }

    // Measured 2026-08-28: the "instance" entry of libxpc's 0x400000cf lookup is a bare 16-byte UUID, and
    // "name" -- the only entry the responder needs -- is serialized after it. Mis-sizing the UUID loses
    // every service lookup in the process.
    TEST(XpcServices, AUuidIsSixteenBytesAndTheEntriesAfterItStillParse)
    {
        const std::vector<uint8_t> instance(16, 0);
        const auto bytes = sogen::xpc::serialize(sogen::xpc::make_dictionary({
            {"handle", sogen::xpc::make_uint64(0)},
            {"instance", {.type_tag = sogen::xpc::type::uuid, .bytes = instance}},
            {"name", sogen::xpc::make_string("com.apple.logd")},
        }));

        uint32_t unsupported_tag = 0;
        const auto parsed = sogen::xpc::parse(bytes, &unsupported_tag);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(unsupported_tag, 0u);
        ASSERT_EQ(parsed->entries.size(), 3u);

        const auto* name = sogen::xpc::find(*parsed, "name");
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(name->text, "com.apple.logd");
    }

    TEST(XpcServices, AValueTagWithNoMeasuredShapeStopsTheWalkAndIsNamed)
    {
        std::vector<uint8_t> bytes = sogen::xpc::serialize(sogen::xpc::make_dictionary({
            {"name", sogen::xpc::make_string("com.apple.logd")},
        }));

        // Turn the string into a tag nothing here decodes; the walk must stop rather than read the bytes
        // that follow it as the next key. The value sits after the magic, the version, the dictionary tag,
        // its size and count, and the NUL-padded key.
        constexpr uint32_t array_tag = 0x0000E000u;
        constexpr size_t value_offset = 8 + 12 + 8;
        sogen::mach::write_u32(bytes, value_offset, array_tag);

        uint32_t unsupported_tag = 0;
        const auto parsed = sogen::xpc::parse(bytes, &unsupported_tag);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(unsupported_tag, array_tag);
        EXPECT_TRUE(parsed->incomplete);
        EXPECT_TRUE(parsed->entries.empty());
    }

    // Measured 2026-08-28 on the host: libxpc stamps 0x10000000 on both asynchronous send forms and
    // 0x40000000 only on the synchronous one. The answer to an asynchronous send goes to the send-once
    // reply port the request carries, which sogen cannot deliver on; answering it on the channel instead
    // is what killed the guest with "BUG IN CLIENT OF LIBDISPATCH: Reply received on unexpected port".
    TEST(XpcServices, AnAsynchronousXpcSendIsNotAnswered)
    {
        const auto emu = macos_test::make_emulator();
        const auto request = sogen::xpc::serialize(sogen::xpc::make_dictionary({}));

        const auto answer = [&](const int32_t id) {
            sogen::mach::msg_call call{};
            call.header = {.bits = sogen::mach::make_bits(sogen::mach::disposition::copy_send, sogen::mach::disposition::make_send_once),
                           .size = static_cast<uint32_t>(sogen::mach::MSG_HEADER_SIZE + request.size()),
                           .remote_port = 0x203,
                           .local_port = 0x70b,
                           .voucher_port = 0,
                           .id = id};
            return sogen::xpc::answer_service_message(*emu, call, request);
        };

        EXPECT_TRUE(answer(0x10000000).empty());
        EXPECT_FALSE(answer(0x40000000).empty());
    }
}
