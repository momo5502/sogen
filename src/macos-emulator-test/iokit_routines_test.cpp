#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/mach_types.hpp>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    constexpr int32_t IO_SERVER_VERSION = 2877;
    constexpr int32_t IO_REGISTRY_ENTRY_FROM_PATH = 2809;
    constexpr int32_t IO_REGISTRY_GET_ROOT_ENTRY = 2827;
    constexpr int32_t IO_SERVICE_GET_MATCHING_SERVICES = 2804;
    constexpr int32_t IO_SERVICE_GET_MATCHING_SERVICE = 2873;
    constexpr int32_t IO_SERVICE_GET_MATCHING_SERVICE_BIN = 2880;
    constexpr int32_t IO_SERVICE_GET_MATCHING_SERVICES_BIN = 2881;
    constexpr int32_t IO_OBJECT_GET_CLASS = 2800;
    constexpr int32_t IO_OBJECT_CONFORMS_TO = 2801;
    constexpr int32_t IO_ITERATOR_NEXT = 2802;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTY = 2805;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_NAME = 2810;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTIES = 2811;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES = 2812;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PATH = 2826;
    constexpr int32_t IO_REGISTRY_ENTRY_IN_PLANE = 2829;
    constexpr int32_t IO_OBJECT_GET_RETAIN_COUNT = 2830;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_NAME_IN_PLANE = 2843;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_REGISTRY_ENTRY_ID = 2871;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN = 2878;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTY_BIN = 2879;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN_BUF = 2888;
    constexpr int32_t IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF = 2889;

    constexpr uint32_t IO_RETURN_NO_RESOURCES = 0xE00002BEu;
    constexpr uint32_t IO_RETURN_IPC_ERROR = 0xE00002BFu;
    constexpr uint32_t IO_RETURN_NO_DEVICE = 0xE00002C0u;
    constexpr uint32_t IO_RETURN_BAD_ARGUMENT = 0xE00002C2u;
    constexpr uint32_t IO_RETURN_UNSUPPORTED = 0xE00002C7u;
    constexpr uint32_t IO_RETURN_NOT_FOUND = 0xE00002F0u;

    constexpr uint64_t GUEST_BUFFER = 0x360000000ULL;

    void append_padded(std::vector<uint8_t>& body, const std::string& bytes)
    {
        body.insert(body.end(), bytes.begin(), bytes.end());
        body.resize(body.size() + ((4 - (bytes.size() % 4)) % 4), 0);
    }

    void append_inline_string(std::vector<uint8_t>& body, const std::string& text)
    {
        const auto base = body.size();
        body.resize(base + 2 * sizeof(uint32_t), 0);
        write_u32(body, base, 0);
        write_u32(body, base + sizeof(uint32_t), static_cast<uint32_t>(text.size() + 1));
        append_padded(body, text + '\0');
    }

    void append_u32(std::vector<uint8_t>& body, const uint32_t value)
    {
        const auto base = body.size();
        body.resize(base + sizeof(uint32_t), 0);
        write_u32(body, base, value);
    }

    void append_u64(std::vector<uint8_t>& body, const uint64_t value)
    {
        const auto base = body.size();
        body.resize(base + sizeof(uint64_t), 0);
        write_u64(body, base, value);
    }

    std::vector<uint8_t> ndr_prefixed()
    {
        return {NDR_RECORD.begin(), NDR_RECORD.end()};
    }

    std::vector<uint8_t> string_body(const std::string& text)
    {
        auto body = ndr_prefixed();
        append_inline_string(body, text);
        return body;
    }

    // The bytes IOServiceMatching(class) puts on the wire: a one-entry binary OSSerialize dictionary
    // whose key is a symbol and whose value is a string carrying kOSSerializeEndCollection.
    std::vector<uint8_t> matching_body(const std::string& class_name)
    {
        std::vector<uint8_t> matching{};
        const auto push = [&matching](const uint32_t token) {
            const auto base = matching.size();
            matching.resize(base + sizeof(uint32_t), 0);
            write_u32(matching, base, token);
        };

        push(0x000000D3u);
        push(0x81000001u);
        push(0x08000000u | static_cast<uint32_t>(std::string{"IOProviderClass"}.size() + 1));
        append_padded(matching, std::string{"IOProviderClass"} + '\0');
        push(0x89000000u | static_cast<uint32_t>(class_name.size()));
        append_padded(matching, class_name);

        auto body = ndr_prefixed();
        append_u32(body, static_cast<uint32_t>(matching.size()));
        body.insert(body.end(), matching.begin(), matching.end());
        return body;
    }

    struct decoded_reply
    {
        msg_header header{};
        std::vector<uint8_t> bytes{};
    };

    decoded_reply call(sogen::macos_emulator& emu, const uint32_t port, const int32_t id, const std::vector<uint8_t>& body,
                       const uint32_t rcv_size = 4096)
    {
        auto reply = macos_test::send_mig_call(emu, port, static_cast<uint32_t>(id), body, rcv_size);
        const auto header = read_msg_header(reply);
        return {.header = header, .bytes = std::move(reply)};
    }

    uint32_t error_code_of(const decoded_reply& reply)
    {
        EXPECT_EQ(reply.header.size, MIG_REPLY_ERROR_SIZE);
        return read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE);
    }

    uint32_t port_of(const decoded_reply& reply)
    {
        EXPECT_NE(reply.header.bits & BITS_COMPLEX, 0u) << "a port reply is complex";
        EXPECT_EQ(reply.header.size, MSG_HEADER_SIZE + MSG_BODY_SIZE + PORT_DESCRIPTOR_SIZE);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE), 1u) << "one descriptor";
        return read_port_descriptor(std::span{reply.bytes}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE)).name;
    }

    std::string counted_string_of(const decoded_reply& reply)
    {
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u) << "RetCode";
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 0u) << "the offset field is always zero";
        const auto count = read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 8);
        const auto* start = reinterpret_cast<const char*>(reply.bytes.data()) + MSG_HEADER_SIZE + NDR_RECORD_SIZE + 12;
        return count == 0 ? std::string{} : std::string{start, count - 1};
    }

    uint32_t scalar_of(const decoded_reply& reply)
    {
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u) << "RetCode";
        return read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4);
    }

    struct ool_reply_fields
    {
        uint64_t address{};
        uint32_t size{};
        uint8_t type{};
        uint64_t inband_count{};
        uint32_t ool_count{};
    };

    ool_reply_fields ool_of(const decoded_reply& reply, const bool has_inband_count)
    {
        EXPECT_NE(reply.header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE), 1u);

        const auto descriptor = read_ool_descriptor(std::span{reply.bytes}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE));
        const auto scalars = MSG_HEADER_SIZE + MSG_BODY_SIZE + OOL_DESCRIPTOR_SIZE + NDR_RECORD_SIZE;

        ool_reply_fields fields{.address = descriptor.address, .size = descriptor.size, .type = descriptor.type};
        if (has_inband_count)
        {
            fields.inband_count = read_u64(reply.bytes, scalars);
            fields.ool_count = read_u32(reply.bytes, scalars + sizeof(uint64_t));
            EXPECT_EQ(reply.header.size, scalars + sizeof(uint64_t) + sizeof(uint32_t));
        }
        else
        {
            fields.ool_count = read_u32(reply.bytes, scalars);
            EXPECT_EQ(reply.header.size, scalars + sizeof(uint32_t));
        }

        return fields;
    }

    std::vector<uint8_t> read_guest(sogen::macos_emulator& emu, const uint64_t address, const size_t size)
    {
        std::vector<uint8_t> bytes(size, 0);
        emu.memory.read_memory(address, bytes.data(), bytes.size());
        return bytes;
    }

    uint32_t platform_expert(sogen::macos_emulator& emu)
    {
        return port_of(call(emu, emu.mach.io_master_port(), IO_REGISTRY_ENTRY_FROM_PATH, string_body("IOService:/")));
    }

    uint32_t registry_root(sogen::macos_emulator& emu)
    {
        return port_of(call(emu, emu.mach.io_master_port(), IO_REGISTRY_GET_ROOT_ENTRY, ndr_prefixed()));
    }

    // A decoder written against the measured format rather than against sogen's encoder, so an encoder
    // that agrees with itself but not with the format still fails.
    struct decoded_entry
    {
        std::string key{};
        uint32_t type{};
        std::string value{};
        bool end{};
    };

    std::vector<decoded_entry> decode_dictionary(const std::span<const uint8_t> bytes)
    {
        EXPECT_GE(bytes.size(), 8u);
        EXPECT_EQ(read_u32(bytes, 0), 0x000000D3u);

        const auto header = read_u32(bytes, 4);
        EXPECT_EQ(header & 0x7F000000u, 0x01000000u) << "top level is a dictionary";
        EXPECT_NE(header & 0x80000000u, 0u) << "the top-level object closes the stream";

        std::vector<decoded_entry> entries{};
        size_t offset = 8;

        for (uint32_t i = 0; i < (header & 0x00FFFFFFu); ++i)
        {
            const auto key_token = read_u32(bytes, offset);
            EXPECT_EQ(key_token & 0x7F000000u, 0x08000000u) << "keys are symbols";
            EXPECT_EQ(key_token & 0x80000000u, 0u) << "a key never carries kOSSerializeEndCollection";
            const auto key_length = key_token & 0x00FFFFFFu;
            offset += 4;
            std::string key{reinterpret_cast<const char*>(bytes.data()) + offset, key_length - 1};
            EXPECT_EQ(bytes[offset + key_length - 1], 0u) << "a symbol's length counts its NUL";
            offset += key_length + ((4 - (key_length % 4)) % 4);

            const auto value_token = read_u32(bytes, offset);
            const auto value_length = value_token & 0x00FFFFFFu;
            offset += 4;
            std::string value{reinterpret_cast<const char*>(bytes.data()) + offset, value_length};
            offset += value_length + ((4 - (value_length % 4)) % 4);

            entries.push_back({.key = std::move(key),
                               .type = value_token & 0x7F000000u,
                               .value = std::move(value),
                               .end = (value_token & 0x80000000u) != 0});
        }

        EXPECT_EQ(offset, bytes.size()) << "the stream ends exactly at the last padded payload";
        return entries;
    }

    TEST(IokitRoutines, ServerVersionIsTheMeasuredStamp)
    {
        const auto emu = macos_test::make_emulator();
        const auto reply = call(*emu, emu->mach.io_master_port(), IO_SERVER_VERSION, ndr_prefixed(), 64);

        EXPECT_EQ(reply.header.id, IO_SERVER_VERSION + 100);
        EXPECT_EQ(reply.header.size, 44u) << "header, NDR, RetCode, uint64 version -- MIG packs to 4";
        EXPECT_EQ(reply.header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u);
        EXPECT_EQ(read_u64(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 0x0134647Aull);
    }

    TEST(IokitRoutines, RegistryEntryFromPathResolvesBothPlaneRoots)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();

        for (const auto* path : {"IOService:/", "IODeviceTree:/"})
        {
            const auto entry = port_of(call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, string_body(path)));
            ASSERT_NE(entry, PORT_NULL) << path;
            EXPECT_EQ(emu->mach.ports.object_of(entry).kind, kernel_object_kind::io_object)
                << "a registry entry is handed over as an io_object port, so handing it back is a lookup";
            EXPECT_EQ(counted_string_of(call(*emu, entry, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOPlatformExpertDevice");
        }
    }

    TEST(IokitRoutines, TheDeviceTreeNodesMobileGestaltOpensByPathResolve)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();

        const auto product = port_of(call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, string_body("IODeviceTree:/product")));
        ASSERT_NE(product, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, product, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOPlatformDevice");
        EXPECT_EQ(counted_string_of(call(*emu, product, IO_REGISTRY_ENTRY_GET_NAME, ndr_prefixed())), "product");
        EXPECT_EQ(counted_string_of(call(*emu, product, IO_REGISTRY_ENTRY_GET_PATH, string_body("IODeviceTree"))), "IODeviceTree:/product")
            << "a child's path is the plane root's path plus its own name";
        EXPECT_EQ(error_code_of(call(*emu, product, IO_REGISTRY_ENTRY_GET_PATH, string_body("IOService"))), IO_RETURN_BAD_ARGUMENT)
            << "the device-tree children are in one plane only";

        const auto chosen = port_of(call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, string_body("IODeviceTree:/chosen")));
        ASSERT_NE(chosen, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, chosen, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOService");

        auto board_id = ndr_prefixed();
        append_inline_string(board_id, "board-id");
        append_u32(board_id, 512);
        const auto reply = call(*emu, chosen, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, board_id);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 4u)
            << "on Apple Silicon board-id is four bytes of OSData on IODeviceTree:/chosen, not a string on the platform expert";
    }

    TEST(IokitRoutines, RegistryEntryFromPathMissesAreANullPortNotAnError)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();

        for (const auto* path :
             {"IOService:/sogen-no-such-entry", "SogenPlane:/x", "not-a-path", "IODeviceTree:/product/deeper", "IOService:/product"})
        {
            const auto reply = call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, string_body(path));
            EXPECT_EQ(port_of(reply), PORT_NULL) << path;
        }
    }

    TEST(IokitRoutines, TheRootEntryIsAboveEveryPlane)
    {
        const auto emu = macos_test::make_emulator();
        const auto root = registry_root(*emu);

        ASSERT_NE(root, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, root, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IORegistryEntry");
        EXPECT_EQ(counted_string_of(call(*emu, root, IO_REGISTRY_ENTRY_GET_NAME, ndr_prefixed())), "Root");
        EXPECT_EQ(scalar_of(call(*emu, root, IO_REGISTRY_ENTRY_IN_PLANE, string_body("IOService"))), 0u);
        EXPECT_EQ(error_code_of(call(*emu, root, IO_REGISTRY_ENTRY_GET_PATH, string_body("IOService"))), IO_RETURN_BAD_ARGUMENT);
    }

    TEST(IokitRoutines, MatchingServiceBinFindsThePlatformExpert)
    {
        const auto emu = macos_test::make_emulator();
        const auto reply =
            call(*emu, emu->mach.io_master_port(), IO_SERVICE_GET_MATCHING_SERVICE_BIN, matching_body("IOPlatformExpertDevice"));

        const auto entry = port_of(reply);
        ASSERT_NE(entry, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, entry, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOPlatformExpertDevice");
    }

    TEST(IokitRoutines, MatchingServiceBinAlsoMatchesASuperclass)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = port_of(call(*emu, emu->mach.io_master_port(), IO_SERVICE_GET_MATCHING_SERVICE_BIN, matching_body("IOService")));

        EXPECT_NE(entry, PORT_NULL);
    }

    TEST(IokitRoutines, MatchingServiceBinMissIsNotFoundWhileMatchingServicesGivesANullIterator)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();

        EXPECT_EQ(error_code_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICE_BIN, matching_body("SogenNoSuchClass"))),
                  IO_RETURN_NOT_FOUND);
        EXPECT_EQ(port_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICES_BIN, matching_body("SogenNoSuchClass"))), PORT_NULL);
    }

    TEST(IokitRoutines, AnIteratorYieldsEveryMatchThenNoDevice)
    {
        const auto emu = macos_test::make_emulator();
        const auto iterator =
            port_of(call(*emu, emu->mach.io_master_port(), IO_SERVICE_GET_MATCHING_SERVICES_BIN, matching_body("IOPlatformExpertDevice")));

        ASSERT_NE(iterator, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, iterator, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOUserIterator");

        const auto first = port_of(call(*emu, iterator, IO_ITERATOR_NEXT, ndr_prefixed()));
        ASSERT_NE(first, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, first, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOPlatformExpertDevice");

        EXPECT_EQ(error_code_of(call(*emu, iterator, IO_ITERATOR_NEXT, ndr_prefixed())), IO_RETURN_NO_DEVICE);
        EXPECT_EQ(error_code_of(call(*emu, iterator, IO_ITERATOR_NEXT, ndr_prefixed())), IO_RETURN_NO_DEVICE)
            << "an exhausted iterator stays exhausted";
    }

    TEST(IokitRoutines, IteratorNextOnSomethingThatIsNotAnIteratorIsBadArgument)
    {
        const auto emu = macos_test::make_emulator();
        EXPECT_EQ(error_code_of(call(*emu, platform_expert(*emu), IO_ITERATOR_NEXT, ndr_prefixed())), IO_RETURN_BAD_ARGUMENT);
    }

    TEST(IokitRoutines, ConformsToWalksTheClassChainAndStopsThere)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        EXPECT_EQ(scalar_of(call(*emu, entry, IO_OBJECT_CONFORMS_TO, string_body("IOPlatformExpertDevice"))), 1u);
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_OBJECT_CONFORMS_TO, string_body("IOService"))), 1u);
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_OBJECT_CONFORMS_TO, string_body("IORegistryEntry"))), 1u);
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_OBJECT_CONFORMS_TO, string_body("IOPlatformDevice"))), 0u)
            << "measured: the host says the platform expert does not conform to IOPlatformDevice";
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_OBJECT_CONFORMS_TO, string_body("SogenBogusClass"))), 0u);
    }

    TEST(IokitRoutines, NamesAndPathsArePerPlane)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        EXPECT_EQ(counted_string_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_NAME, ndr_prefixed())), "J516mAP");
        EXPECT_EQ(counted_string_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_NAME_IN_PLANE, string_body("IODeviceTree"))), "device-tree");
        EXPECT_EQ(counted_string_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PATH, string_body("IOService"))), "IOService:/");
        EXPECT_EQ(counted_string_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PATH, string_body("IODeviceTree"))), "IODeviceTree:/");
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PATH, string_body("SogenPlane"))), IO_RETURN_BAD_ARGUMENT);
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_REGISTRY_ENTRY_IN_PLANE, string_body("IODeviceTree"))), 1u);
        EXPECT_EQ(scalar_of(call(*emu, entry, IO_REGISTRY_ENTRY_IN_PLANE, string_body("SogenPlane"))), 0u);
    }

    TEST(IokitRoutines, RegistryEntryIdAndRetainCountAreAnswered)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        const auto id_reply = call(*emu, entry, IO_REGISTRY_ENTRY_GET_REGISTRY_ENTRY_ID, ndr_prefixed());
        EXPECT_EQ(id_reply.header.size, 44u);
        EXPECT_EQ(read_u32(id_reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u);
        EXPECT_EQ(read_u64(id_reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 0x100000200ull);

        EXPECT_GT(scalar_of(call(*emu, entry, IO_OBJECT_GET_RETAIN_COUNT, ndr_prefixed())), 0u);
    }

    TEST(IokitRoutines, PropertyBytesTerminatesAStringAndNotAData)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        auto body = ndr_prefixed();
        append_inline_string(body, "model");
        append_u32(body, 512);

        const auto data_reply = call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, body);
        EXPECT_EQ(read_u32(data_reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE), 0u);
        EXPECT_EQ(read_u32(data_reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 9u) << "\"Mac15,11\" and its NUL";
        EXPECT_EQ(data_reply.header.size, 52u);

        auto string_request = ndr_prefixed();
        append_inline_string(string_request, "IOPlatformSerialNumber");
        append_u32(string_request, 512);

        const auto string_reply = call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, string_request);
        EXPECT_EQ(read_u32(string_reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 11u)
            << "measured: an OSString comes back with the terminator the serialised form leaves off";
    }

    TEST(IokitRoutines, PropertyBytesMissesUseTheCodesTheKernelUses)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        auto missing = ndr_prefixed();
        append_inline_string(missing, "sogen-no-such-property");
        append_u32(missing, 512);
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, missing)), IO_RETURN_NO_RESOURCES)
            << "2812 answers NoResources for a miss, not NotFound -- 2889 is the one that answers NotFound";

        auto cramped = ndr_prefixed();
        append_inline_string(cramped, "model");
        append_u32(cramped, 4);
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, cramped)), IO_RETURN_IPC_ERROR);
    }

    std::vector<uint8_t> property_bin_buf_body(const std::string& key, const uint64_t buffer, const uint64_t size)
    {
        auto body = ndr_prefixed();
        append_inline_string(body, "");
        append_inline_string(body, key);
        append_u32(body, 0);
        append_u64(body, buffer);
        append_u64(body, size);
        return body;
    }

    TEST(IokitRoutines, PropertyBinBufWritesAppleSExactBytesInband)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(GUEST_BUFFER, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        const auto entry = platform_expert(*emu);

        const auto data_reply =
            call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF, property_bin_buf_body("model", GUEST_BUFFER, 2048));
        const auto data_fields = ool_of(data_reply, true);
        EXPECT_EQ(data_fields.address, 0u) << "an answer that fits is written into the caller's buffer";
        EXPECT_EQ(data_fields.size, 0u);
        EXPECT_EQ(data_fields.type, descriptor_type::ool);
        EXPECT_EQ(data_fields.inband_count, 20u);
        EXPECT_EQ(data_fields.ool_count, 0u);

        // IOCFSerialize(CFData("Mac15,11\0"), kIOCFSerializeToBinary) on 25G76, byte for byte.
        const std::vector<uint8_t> expected_data{0xd3, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x8a, 0x4d, 0x61,
                                                 0x63, 0x31, 0x35, 0x2c, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00};
        EXPECT_EQ(read_guest(*emu, GUEST_BUFFER, expected_data.size()), expected_data);

        const auto string_reply =
            call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF, property_bin_buf_body("IOPlatformSerialNumber", GUEST_BUFFER, 2048));
        EXPECT_EQ(ool_of(string_reply, true).inband_count, 20u);

        // IOCFSerialize(CFString("SOGEN00001"), kIOCFSerializeToBinary): a string's length leaves the NUL
        // out where a data's counts it, which is why both come to 20 bytes from different lengths.
        const std::vector<uint8_t> expected_string{0xd3, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x89, 0x53, 0x4f,
                                                   0x47, 0x45, 0x4e, 0x30, 0x30, 0x30, 0x30, 0x31, 0x00, 0x00};
        EXPECT_EQ(read_guest(*emu, GUEST_BUFFER, expected_string.size()), expected_string);
    }

    TEST(IokitRoutines, PropertyBinBufFallsBackOutOfLineWhenTheBufferIsTooSmall)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(GUEST_BUFFER, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        const auto reply =
            call(*emu, platform_expert(*emu), IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF, property_bin_buf_body("model", GUEST_BUFFER, 16));
        const auto fields = ool_of(reply, true);

        EXPECT_NE(fields.address, 0u);
        EXPECT_EQ(fields.size, 20u);
        EXPECT_EQ(fields.inband_count, 0u) << "the inband count is zero once the answer went out of line";
        EXPECT_EQ(fields.ool_count, 20u);

        const std::vector<uint8_t> expected{0xd3, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x8a, 0x4d, 0x61,
                                            0x63, 0x31, 0x35, 0x2c, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00};
        EXPECT_EQ(read_guest(*emu, fields.address, expected.size()), expected);
    }

    TEST(IokitRoutines, PropertyBinBufAndBinMissWithNotFound)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(GUEST_BUFFER, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        const auto entry = platform_expert(*emu);

        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF,
                                     property_bin_buf_body("sogen-no-such-property", GUEST_BUFFER, 2048))),
                  IO_RETURN_NOT_FOUND);

        auto bin_body = ndr_prefixed();
        append_inline_string(bin_body, "");
        append_inline_string(bin_body, "sogen-no-such-property");
        append_u32(bin_body, 0);
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BIN, bin_body)), IO_RETURN_NOT_FOUND);
    }

    TEST(IokitRoutines, PropertyBinCarriesTheSameBytesOutOfLine)
    {
        const auto emu = macos_test::make_emulator();

        auto body = ndr_prefixed();
        append_inline_string(body, "");
        append_inline_string(body, "model");
        append_u32(body, 0);

        const auto fields = ool_of(call(*emu, platform_expert(*emu), IO_REGISTRY_ENTRY_GET_PROPERTY_BIN, body), false);
        EXPECT_NE(fields.address, 0u);
        EXPECT_EQ(fields.size, 20u);
        EXPECT_EQ(fields.ool_count, 20u);

        const std::vector<uint8_t> expected{0xd3, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x8a, 0x4d, 0x61,
                                            0x63, 0x31, 0x35, 0x2c, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00};
        EXPECT_EQ(read_guest(*emu, fields.address, expected.size()), expected);
    }

    TEST(IokitRoutines, PropertiesBinBufSerializesTheWholeDictionary)
    {
        const auto emu = macos_test::make_emulator();
        emu->memory.allocate_memory(GUEST_BUFFER, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        auto body = ndr_prefixed();
        append_u64(body, GUEST_BUFFER);
        append_u64(body, 4096);

        const auto fields = ool_of(call(*emu, platform_expert(*emu), IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN_BUF, body), true);
        ASSERT_EQ(fields.address, 0u);
        ASSERT_GT(fields.inband_count, 0u);
        EXPECT_EQ(fields.ool_count, 0u);

        const auto serialized = read_guest(*emu, GUEST_BUFFER, static_cast<size_t>(fields.inband_count));
        const auto entries = decode_dictionary(serialized);

        ASSERT_EQ(entries.size(), 16u);
        EXPECT_TRUE(entries.back().end) << "only the last value closes the collection";
        for (size_t i = 0; i + 1 < entries.size(); ++i)
        {
            EXPECT_FALSE(entries[i].end) << entries[i].key;
        }

        const auto find = [&entries](const std::string& key) -> const decoded_entry* {
            const auto match = std::ranges::find(entries, key, &decoded_entry::key);
            return match == entries.end() ? nullptr : &*match;
        };

        ASSERT_NE(find("model"), nullptr);
        EXPECT_EQ(find("model")->type, 0x0A000000u) << "model is OSData on the host, not OSString";
        EXPECT_EQ(find("model")->value, std::string("Mac15,11\0", 9));

        ASSERT_NE(find("IOPlatformSerialNumber"), nullptr);
        EXPECT_EQ(find("IOPlatformSerialNumber")->type, 0x09000000u);
        EXPECT_EQ(find("IOPlatformSerialNumber")->value, "SOGEN00001");

        ASSERT_NE(find("IOPlatformUUID"), nullptr);
        EXPECT_EQ(find("IOPlatformUUID")->type, 0x09000000u);
        EXPECT_EQ(find("IOPlatformUUID")->value.size(), 36u);

        ASSERT_NE(find("manufacturer"), nullptr);
        EXPECT_EQ(find("manufacturer")->value, std::string("Apple Inc.\0", 11));

        ASSERT_NE(find("serial-number"), nullptr);
        EXPECT_EQ(find("serial-number")->value.size(), 32u) << "the device-tree serial is a fixed 32-byte field";

        ASSERT_NE(find("clock-frequency"), nullptr);
        EXPECT_EQ(find("clock-frequency")->value.size(), 4u);

        EXPECT_EQ(find("board-id"), nullptr) << "Apple Silicon has none; target-type and target-sub-type stand in";
        ASSERT_NE(find("target-sub-type"), nullptr);
        EXPECT_EQ(find("target-sub-type")->value, std::string("J516mAP\0", 8));
    }

    TEST(IokitRoutines, PropertiesOfTheRootAreTheEmptyDictionary)
    {
        const auto emu = macos_test::make_emulator();

        const auto fields = ool_of(call(*emu, registry_root(*emu), IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN, ndr_prefixed()), false);
        ASSERT_EQ(fields.size, 8u);

        const std::vector<uint8_t> expected{0xd3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81};
        EXPECT_EQ(read_guest(*emu, fields.address, expected.size()), expected);
    }

    TEST(IokitRoutines, TheLegacyPropertyRoutineAnswersTheMeasuredXmlFragment)
    {
        const auto emu = macos_test::make_emulator();
        const auto entry = platform_expert(*emu);

        const auto data_fields = ool_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY, string_body("model")), false);
        ASSERT_EQ(data_fields.size, 33u) << "measured on the host, terminator included";
        const auto data_bytes = read_guest(*emu, data_fields.address, data_fields.size);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(data_bytes.data())), "<data ID=\"0\">TWFjMTUsMTEA</data>");

        const auto string_fields = ool_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY, string_body("IOPlatformSerialNumber")), false);
        ASSERT_EQ(string_fields.size, 35u);
        const auto string_bytes = read_guest(*emu, string_fields.address, string_fields.size);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(string_bytes.data())), "<string ID=\"0\">SOGEN00001</string>");

        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY, string_body("sogen-no-such-property"))),
                  IO_RETURN_NOT_FOUND);
    }

    TEST(IokitRoutines, TheRoutinesTheHostKernelRefusesAreRefusedHereToo)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();

        EXPECT_EQ(error_code_of(call(*emu, platform_expert(*emu), IO_REGISTRY_ENTRY_GET_PROPERTIES, ndr_prefixed())),
                  IO_RETURN_UNSUPPORTED);
        EXPECT_EQ(error_code_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICE,
                                     string_body("<dict><key>IOProviderClass</key><string>IOPlatformExpertDevice</string></dict>"))),
                  IO_RETURN_UNSUPPORTED);
        EXPECT_EQ(error_code_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICES,
                                     string_body("<dict><key>IOProviderClass</key><string>IOPlatformExpertDevice</string></dict>"))),
                  IO_RETURN_UNSUPPORTED);
    }

    TEST(IokitRoutines, APortThatNamesNoRegistryObjectIsBadArgument)
    {
        const auto emu = macos_test::make_emulator();

        EXPECT_EQ(error_code_of(call(*emu, emu->mach.io_master_port(), IO_OBJECT_GET_CLASS, ndr_prefixed())), IO_RETURN_BAD_ARGUMENT)
            << "measured: IOObjectGetClass on the master port answers kIOReturnBadArgument";

        const auto stale = emu->mach.ports.allocate_receive_right({.kind = kernel_object_kind::io_object, .id = 0xDEADBEEF});
        EXPECT_EQ(error_code_of(call(*emu, stale, IO_OBJECT_GET_CLASS, ndr_prefixed())), IO_RETURN_BAD_ARGUMENT);
        EXPECT_EQ(error_code_of(call(*emu, stale, IO_REGISTRY_ENTRY_GET_NAME, ndr_prefixed())), IO_RETURN_BAD_ARGUMENT);
        EXPECT_EQ(error_code_of(call(*emu, stale, IO_REGISTRY_ENTRY_GET_PATH, string_body("IOService"))), IO_RETURN_BAD_ARGUMENT);
        EXPECT_EQ(error_code_of(call(*emu, stale, IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN, ndr_prefixed())), IO_RETURN_BAD_ARGUMENT);
    }

    TEST(IokitRoutines, MalformedRequestsAreRefusedRatherThanRead)
    {
        const auto emu = macos_test::make_emulator();
        const auto master = emu->mach.io_master_port();
        const auto entry = platform_expert(*emu);

        EXPECT_EQ(error_code_of(call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, ndr_prefixed())),
                  static_cast<uint32_t>(mig_error::bad_arguments))
            << "a c_string argument with no offset or count at all";

        auto truncated_count = ndr_prefixed();
        append_u32(truncated_count, 0);
        append_u32(truncated_count, 64);
        append_padded(truncated_count, "IOService:/");
        EXPECT_EQ(error_code_of(call(*emu, master, IO_REGISTRY_ENTRY_FROM_PATH, truncated_count)),
                  static_cast<uint32_t>(mig_error::bad_arguments))
            << "a count that runs past the end of the message";

        auto oversized_matching = ndr_prefixed();
        append_u32(oversized_matching, 0x1000);
        EXPECT_EQ(error_code_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICE_BIN, oversized_matching)),
                  static_cast<uint32_t>(mig_error::bad_arguments));

        auto empty_matching = ndr_prefixed();
        append_u32(empty_matching, 0);
        EXPECT_EQ(error_code_of(call(*emu, master, IO_SERVICE_GET_MATCHING_SERVICE_BIN, empty_matching)), IO_RETURN_NOT_FOUND)
            << "an unparseable matching dictionary matches nothing rather than matching everything";

        auto short_bin_buf = ndr_prefixed();
        append_inline_string(short_bin_buf, "");
        append_inline_string(short_bin_buf, "model");
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BIN_BUF, short_bin_buf)),
                  static_cast<uint32_t>(mig_error::bad_arguments))
            << "no options, no buffer and no buffer size";

        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTIES_BIN_BUF, ndr_prefixed())),
                  static_cast<uint32_t>(mig_error::bad_arguments));

        auto no_size = ndr_prefixed();
        append_inline_string(no_size, "model");
        EXPECT_EQ(error_code_of(call(*emu, entry, IO_REGISTRY_ENTRY_GET_PROPERTY_BYTES, no_size)),
                  static_cast<uint32_t>(mig_error::bad_arguments));
    }

    TEST(IokitRoutines, AMatchingKeyTheModelCannotEvaluateMatchesNothing)
    {
        const auto emu = macos_test::make_emulator();

        std::vector<uint8_t> matching{};
        const auto push = [&matching](const uint32_t token) {
            const auto base = matching.size();
            matching.resize(base + sizeof(uint32_t), 0);
            write_u32(matching, base, token);
        };

        push(0x000000D3u);
        push(0x81000001u);
        push(0x08000000u | 16u);
        append_padded(matching, std::string("IOResourceMatch") + '\0');
        push(0x89000000u | 4u);
        append_padded(matching, "boot");

        auto body = ndr_prefixed();
        append_u32(body, static_cast<uint32_t>(matching.size()));
        body.insert(body.end(), matching.begin(), matching.end());

        EXPECT_EQ(error_code_of(call(*emu, emu->mach.io_master_port(), IO_SERVICE_GET_MATCHING_SERVICE_BIN, body)), IO_RETURN_NOT_FOUND);
    }
}
