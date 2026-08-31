#include <gtest/gtest.h>

#include "macos_test_utils.hpp"

#include <mach/io_surface_user_client.hpp>
#include <mach/mach_types.hpp>

#include <string>
#include <vector>

namespace
{
    using namespace sogen::mach; // NOLINT(google-build-using-namespace)

    constexpr int32_t IO_OBJECT_GET_CLASS = 2800;
    constexpr int32_t IO_OBJECT_CONFORMS_TO = 2801;
    constexpr int32_t IO_ITERATOR_NEXT = 2802;
    constexpr int32_t IO_REGISTRY_ENTRY_FROM_PATH = 2809;
    constexpr int32_t IO_SERVICE_GET_MATCHING_SERVICES_BIN = 2881;
    constexpr int32_t IO_SERVICE_OPEN_EXTENDED = 2862;
    constexpr int32_t IO_CONNECT_METHOD = 2865;

    constexpr uint32_t IO_RETURN_NO_MEMORY = 0xE00002BDu;
    constexpr uint32_t IO_RETURN_BAD_ARGUMENT = 0xE00002C2u;
    constexpr uint32_t IO_RETURN_UNSUPPORTED = 0xE00002C7u;

    constexpr uint32_t SELECTOR_CREATE = 0;
    constexpr uint32_t SELECTOR_SET_VALUE = 9;
    constexpr uint32_t SELECTOR_COPY_ALL_VALUES = 10;
    constexpr uint32_t SELECTOR_GET_LIMITS = 13;
    constexpr uint32_t SELECTOR_SET_PURGEABLE = 20;
    constexpr uint32_t SELECTOR_CREATE_MACH_PORT = 35;

    // mov x16, #-100 -- the word IOKit's iokit_user_client_trap stub starts with.
    constexpr uint32_t movn_x16_iokit_user_client = 0x92800C70;
    constexpr uint32_t svc_80 = 0xD4001001;

    constexpr uint64_t TRAP_CODE_BASE = 0x100000000ULL + 64 * sogen::MACOS_PAGE_SIZE;
    constexpr uint64_t SCRATCH = 0x340100000ULL;

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

    void append_padded(std::vector<uint8_t>& body, const std::span<const uint8_t> bytes)
    {
        body.insert(body.end(), bytes.begin(), bytes.end());
        body.resize(body.size() + ((4 - (bytes.size() % 4)) % 4), 0);
    }

    void append_inline_string(std::vector<uint8_t>& body, const std::string& text)
    {
        append_u32(body, 0);
        append_u32(body, static_cast<uint32_t>(text.size() + 1));
        const auto terminated = text + '\0';
        append_padded(body, {reinterpret_cast<const uint8_t*>(terminated.data()), terminated.size()});
    }

    template <typename T>
    T read_guest(sogen::macos_emulator& emu, const uint64_t address)
    {
        T value{};
        EXPECT_TRUE(emu.memory.try_read_memory(address, &value, sizeof(value)));
        return value;
    }

    std::vector<uint8_t> ndr_prefixed()
    {
        return {NDR_RECORD.begin(), NDR_RECORD.end()};
    }

    // A binary OSSerialize writer built from the measured format rather than from sogen's own encoder, so
    // an encoder that agrees with itself and not with IOKit still fails here.
    struct serializer
    {
        std::vector<uint8_t> bytes{};

        serializer()
        {
            append_u32(this->bytes, 0x000000D3u);
        }

        void token(const uint32_t type, const uint32_t length, const bool end)
        {
            append_u32(this->bytes, (type << 24) | (end ? 0x80000000u : 0u) | length);
        }

        void symbol(const std::string& text)
        {
            this->token(0x08, static_cast<uint32_t>(text.size() + 1), false);
            const auto terminated = text + '\0';
            append_padded(this->bytes, {reinterpret_cast<const uint8_t*>(terminated.data()), terminated.size()});
        }

        void string(const std::string& text, const bool end)
        {
            this->token(0x09, static_cast<uint32_t>(text.size()), end);
            append_padded(this->bytes, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        }

        void number(const uint64_t value, const uint32_t bits, const bool end)
        {
            this->token(0x04, bits, end);
            append_u64(this->bytes, value);
        }

        void reference(const uint32_t index, const bool end)
        {
            this->token(0x0C, index, end);
        }
    };

    struct decoded_reply
    {
        msg_header header{};
        std::vector<uint8_t> bytes{};
    };

    decoded_reply call(sogen::macos_emulator& emu, const uint32_t port, const int32_t id, const std::vector<uint8_t>& body,
                       const uint32_t rcv_size = 8192)
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
        EXPECT_NE(reply.header.bits & BITS_COMPLEX, 0u);
        EXPECT_EQ(read_u32(reply.bytes, MSG_HEADER_SIZE), 1u);
        return read_port_descriptor(std::span{reply.bytes}.subspan(MSG_HEADER_SIZE + MSG_BODY_SIZE)).name;
    }

    std::string counted_string_of(const decoded_reply& reply)
    {
        const auto count = read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 8);
        const auto* start = reinterpret_cast<const char*>(reply.bytes.data()) + MSG_HEADER_SIZE + NDR_RECORD_SIZE + 12;
        return count == 0 ? std::string{} : std::string{start, count - 1};
    }

    struct method_call
    {
        uint32_t selector{};
        std::vector<uint64_t> scalars{};
        std::vector<uint8_t> inband{};
        uint32_t inband_output_max{};
        uint32_t scalar_output_max{};
        uint64_t ool_output{};
        uint64_t ool_output_max{};
    };

    std::vector<uint8_t> method_body(const method_call& method)
    {
        auto body = ndr_prefixed();
        append_u32(body, method.selector);
        append_u32(body, static_cast<uint32_t>(method.scalars.size()));
        for (const auto scalar : method.scalars)
        {
            append_u64(body, scalar);
        }

        append_u32(body, static_cast<uint32_t>(method.inband.size()));
        append_padded(body, method.inband);
        append_u64(body, 0);
        append_u64(body, 0);
        append_u32(body, method.inband_output_max);
        append_u32(body, method.scalar_output_max);
        append_u64(body, method.ool_output);
        append_u64(body, method.ool_output_max);
        return body;
    }

    struct method_reply
    {
        uint32_t code{};
        std::vector<uint8_t> inband{};
        std::vector<uint64_t> scalars{};
        uint64_t ool_output_size{};
    };

    method_reply decode_method_reply(const decoded_reply& reply)
    {
        method_reply decoded{};

        if (reply.header.size == MIG_REPLY_ERROR_SIZE)
        {
            decoded.code = read_u32(reply.bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE);
            return decoded;
        }

        size_t offset = MSG_HEADER_SIZE + NDR_RECORD_SIZE;
        decoded.code = read_u32(reply.bytes, offset);
        offset += sizeof(uint32_t);

        const auto inband_count = read_u32(reply.bytes, offset);
        offset += sizeof(uint32_t);
        decoded.inband.assign(reply.bytes.begin() + static_cast<ptrdiff_t>(offset),
                              reply.bytes.begin() + static_cast<ptrdiff_t>(offset + inband_count));
        offset += inband_count + ((4 - (inband_count % 4)) % 4);

        const auto scalar_count = read_u32(reply.bytes, offset);
        offset += sizeof(uint32_t);
        for (uint32_t i = 0; i < scalar_count; ++i)
        {
            decoded.scalars.push_back(read_u64(reply.bytes, offset));
            offset += sizeof(uint64_t);
        }

        decoded.ool_output_size = read_u64(reply.bytes, offset);
        offset += sizeof(uint64_t);

        EXPECT_EQ(reply.header.size, offset) << "the reply is exactly its fields";
        return decoded;
    }

    uint32_t io_surface_root(sogen::macos_emulator& emu)
    {
        return port_of(call(emu, emu.mach.io_master_port(), IO_REGISTRY_ENTRY_FROM_PATH, [] {
            auto body = ndr_prefixed();
            append_inline_string(body, "IOService:/IOSurfaceRoot");
            return body;
        }()));
    }

    uint32_t open_connection(sogen::macos_emulator& emu, const uint32_t service)
    {
        auto body = ndr_prefixed();
        append_u32(body, 0);
        body.insert(body.end(), NDR_RECORD.begin(), NDR_RECORD.end());
        append_u32(body, 0);
        return port_of(call(emu, service, IO_SERVICE_OPEN_EXTENDED, body));
    }

    uint32_t open_surface_connection(sogen::macos_emulator& emu)
    {
        return open_connection(emu, io_surface_root(emu));
    }

    std::vector<uint8_t> properties_of(const uint64_t width, const uint64_t height, const uint64_t bytes_per_row,
                                       const uint32_t pixel_format, const uint32_t bytes_per_element)
    {
        serializer out{};
        out.token(0x01, 5, true);
        out.symbol("IOSurfaceWidth");
        out.number(width, 32, false);
        out.symbol("IOSurfaceHeight");
        out.number(height, 32, false);
        out.symbol("IOSurfaceBytesPerRow");
        out.number(bytes_per_row, 32, false);
        out.symbol("IOSurfacePixelFormat");
        out.number(pixel_format, 32, false);
        out.symbol("IOSurfaceBytesPerElement");
        out.number(bytes_per_element, 32, true);
        return out.bytes;
    }

    method_reply create_surface(sogen::macos_emulator& emu, const uint32_t connection, const std::vector<uint8_t>& properties)
    {
        return decode_method_reply(call(
            emu, connection, IO_CONNECT_METHOD,
            method_body({.selector = SELECTOR_CREATE, .scalars = {0}, .inband = properties, .inband_output_max = IO_SURFACE_RECORD_SIZE}),
            8192));
    }

    class trap_runner
    {
      public:
        explicit trap_runner(sogen::macos_emulator& emu)
            : emu_(&emu)
        {
        }

        uint64_t operator()(const std::vector<uint64_t>& arguments)
        {
            std::vector<uint32_t> words{};
            for (uint32_t reg = 0; reg < arguments.size(); ++reg)
            {
                macos_test::load_x(words, reg, arguments[reg]);
            }

            words.push_back(movn_x16_iokit_user_client);
            words.push_back(svc_80);

            macos_test::write_guest_code(*this->emu_, this->next_base_, words);
            this->next_base_ += sogen::MACOS_PAGE_SIZE;
            this->emu_->start(words.size());

            return this->emu_->emu().reg(sogen::arm64_register::x0);
        }

      private:
        sogen::macos_emulator* emu_;
        uint64_t next_base_{TRAP_CODE_BASE};
    };

    TEST(IoSurface, TheRootServiceIsMatchedByNameAndOpensAUserClient)
    {
        const auto emu = macos_test::make_emulator();

        serializer matching{};
        matching.token(0x01, 1, true);
        matching.symbol("IONameMatch");
        matching.string("IOSurfaceRoot", true);

        auto body = ndr_prefixed();
        append_u32(body, static_cast<uint32_t>(matching.bytes.size()));
        body.insert(body.end(), matching.bytes.begin(), matching.bytes.end());

        const auto iterator = port_of(call(*emu, emu->mach.io_master_port(), IO_SERVICE_GET_MATCHING_SERVICES_BIN, body));
        ASSERT_NE(iterator, PORT_NULL) << "_iosConnectInitalize aborts the process when this match finds nothing";

        const auto service = port_of(call(*emu, iterator, IO_ITERATOR_NEXT, ndr_prefixed()));
        ASSERT_NE(service, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, service, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOSurfaceRoot");

        auto conforms = ndr_prefixed();
        append_inline_string(conforms, "IOService");
        EXPECT_EQ(read_u32(call(*emu, service, IO_OBJECT_CONFORMS_TO, conforms).bytes, MSG_HEADER_SIZE + NDR_RECORD_SIZE + 4), 1u);

        const auto connection = open_connection(*emu, service);
        ASSERT_NE(connection, PORT_NULL);
        EXPECT_EQ(counted_string_of(call(*emu, connection, IO_OBJECT_GET_CLASS, ndr_prefixed())), "IOSurfaceRootUserClient");
    }

    TEST(IoSurface, OpeningAServiceWithNoUserClientIsRefused)
    {
        const auto emu = macos_test::make_emulator();

        auto path = ndr_prefixed();
        append_inline_string(path, "IOService:/");
        const auto platform_expert = port_of(call(*emu, emu->mach.io_master_port(), IO_REGISTRY_ENTRY_FROM_PATH, path));

        auto body = ndr_prefixed();
        append_u32(body, 0);
        body.insert(body.end(), NDR_RECORD.begin(), NDR_RECORD.end());
        append_u32(body, 0);

        EXPECT_EQ(error_code_of(call(*emu, platform_expert, IO_SERVICE_OPEN_EXTENDED, body)), IO_RETURN_UNSUPPORTED)
            << "sogen runs one user client, and an open that succeeds on a service with no methods is worse than a refusal";
    }

    TEST(IoSurface, TheLimitsAreTheMeasuredFortyBytes)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);

        const auto reply = decode_method_reply(
            call(*emu, connection, IO_CONNECT_METHOD, method_body({.selector = SELECTOR_GET_LIMITS, .inband_output_max = 40})));

        ASSERT_EQ(reply.code, 0u);
        ASSERT_EQ(reply.inband.size(), 40u);
        EXPECT_EQ(read_u64(reply.inband, 0), 0x0000007F00003FFFull);
        EXPECT_EQ(read_u64(reply.inband, 8), 0x8000ull) << "IOSurfaceGetPropertyMaximum(kIOSurfaceBytesPerRow)";
        EXPECT_EQ(read_u64(reply.inband, 16), 0x4000ull) << "IOSurfaceGetPropertyMaximum(kIOSurfaceWidth)";
        EXPECT_EQ(read_u64(reply.inband, 24), 0x4000ull) << "IOSurfaceGetPropertyMaximum(kIOSurfaceHeight)";
        EXPECT_EQ(read_u64(reply.inband, 32), 0x10000ull);
    }

    TEST(IoSurface, CreateMapsABufferAndFillsTheRecordTheClientReadsBack)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);

        const auto reply = create_surface(*emu, connection, properties_of(320, 232, 1280, 0x42475241, 4));

        ASSERT_EQ(reply.code, 0u);
        ASSERT_EQ(reply.inband.size(), IO_SURFACE_RECORD_SIZE) << "the client copies the whole record into its IOSurfaceClient at +0x70";

        const auto base = read_u64(reply.inband, 0x00);
        const auto info = read_u64(reply.inband, 0x08);
        EXPECT_NE(base, 0u);
        EXPECT_NE(info, 0u);
        EXPECT_EQ(read_u32(reply.inband, 0x18), 1u) << "the first surface of the run";
        EXPECT_EQ(read_u64(reply.inband, 0x20), 320ull * 232 * 4) << "allocSize derived from the stride and the height";
        EXPECT_EQ(read_u64(reply.inband, 0x28), 320ull);
        EXPECT_EQ(read_u64(reply.inband, 0x30), 232ull);
        EXPECT_EQ(read_u64(reply.inband, 0x38), 1280ull);
        EXPECT_EQ(read_u64(reply.inband, 0x40), 0ull) << "IOSurfaceGetBaseAddress adds this offset to the base";
        EXPECT_EQ(read_u32(reply.inband, 0x48), 0x42475241u);
        EXPECT_EQ(read_u64(reply.inband, 0x58), 0ull) << "single plane";
        EXPECT_EQ(read_u32(reply.inband, 0x60) & 0xFFFFu, 4u);
        EXPECT_EQ(reply.inband[0x62], 1) << "element width";
        EXPECT_EQ(reply.inband[0x63], 1) << "element height";

        const std::array<uint8_t, 4> written{0x11, 0x22, 0x33, 0x44};
        ASSERT_TRUE(emu->memory.try_write_memory(base + 320ull * 232 * 4 - written.size(), written.data(), written.size()))
            << "the last pixel of the surface is mapped and writable";

        std::array<uint8_t, 4> read_back{};
        ASSERT_TRUE(emu->memory.try_read_memory(base, read_back.data(), read_back.size()));
        EXPECT_EQ(read_back, (std::array<uint8_t, 4>{}));
    }

    // The dictionary CoreUI sends has its height stored as kOSSerializeObject: a back-reference to the
    // number the width already used. Resolving the index against the values only -- or one-based --
    // reads a key symbol as the height and produces a surface with no pixels.
    TEST(IoSurface, ABackReferenceResolvesToTheObjectAtThatParseIndex)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);

        serializer out{};
        out.token(0x01, 3, true);
        out.symbol("IOSurfaceBytesPerElement"); // object 1
        out.number(4, 32, false);               // object 2
        out.symbol("IOSurfaceWidth");           // object 3
        out.number(240, 32, false);             // object 4
        out.symbol("IOSurfaceHeight");          // object 5
        out.reference(4, true);

        const auto reply = create_surface(*emu, connection, out.bytes);

        ASSERT_EQ(reply.code, 0u);
        EXPECT_EQ(read_u64(reply.inband, 0x28), 240ull);
        EXPECT_EQ(read_u64(reply.inband, 0x30), 240ull) << "the back-reference is the width's number, not the height's key";
        EXPECT_EQ(read_u64(reply.inband, 0x38), 240ull * 4) << "a dictionary with no stride gets the derived one";
    }

    TEST(IoSurface, ASurfaceWithNoPixelsIsRefusedRatherThanMapped)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);

        serializer out{};
        out.token(0x01, 1, true);
        out.symbol("IOSurfaceWidth");
        out.number(320, 32, true);

        EXPECT_EQ(create_surface(*emu, connection, out.bytes).code, IO_RETURN_NO_MEMORY);
        EXPECT_EQ(emu->ui.surfaces.live_count(), 0u);
    }

    TEST(IoSurface, SetValueTakesTheKeyFromTheSecondElementAndCopyAllValuesGivesItBack)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);

        serializer value{};
        value.token(0x02, 2, true);
        value.string("CoreUI image IOSurface", false);
        value.string("IOSurfaceName", true);

        std::vector<uint8_t> inband{};
        append_u32(inband, id);
        append_u64(inband, 0);
        inband.insert(inband.end(), value.bytes.begin(), value.bytes.end());

        const auto set = decode_method_reply(call(*emu, connection, IO_CONNECT_METHOD,
                                                  method_body({.selector = SELECTOR_SET_VALUE, .inband = inband, .inband_output_max = 4})));
        ASSERT_EQ(set.code, 0u);
        ASSERT_EQ(set.inband.size(), 4u);

        emu->memory.allocate_memory(SCRATCH, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);

        std::vector<uint8_t> lookup{};
        append_u32(lookup, id);
        append_u64(lookup, 0);
        append_u32(lookup, 0);

        const auto copied = decode_method_reply(call(*emu, connection, IO_CONNECT_METHOD,
                                                     method_body({.selector = SELECTOR_COPY_ALL_VALUES,
                                                                  .inband = lookup,
                                                                  .ool_output = SCRATCH,
                                                                  .ool_output_max = sogen::MACOS_PAGE_SIZE})));
        ASSERT_EQ(copied.code, 0u);
        ASSERT_GT(copied.ool_output_size, 0u);

        std::vector<uint8_t> serialized(copied.ool_output_size, 0);
        emu->memory.read_memory(SCRATCH, serialized.data(), serialized.size());

        const auto entries = parse_binary_dictionary(serialized);
        ASSERT_TRUE(entries.has_value());

        bool found = false;
        for (const auto& entry : *entries)
        {
            if (entry.key == "IOSurfaceName")
            {
                found = true;
                EXPECT_EQ(entry.value.text, "CoreUI image IOSurface");
            }
        }

        EXPECT_TRUE(found) << "the array is [value, key]; reading it the other way round stores the value under itself";
    }

    TEST(IoSurface, SetPurgeableAnswersThePreviousState)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);

        const auto first =
            decode_method_reply(call(*emu, connection, IO_CONNECT_METHOD,
                                     method_body({.selector = SELECTOR_SET_PURGEABLE, .scalars = {id, 3}, .scalar_output_max = 1})));
        ASSERT_EQ(first.code, 0u);
        ASSERT_EQ(first.scalars.size(), 1u);
        EXPECT_EQ(first.scalars[0], 0u);

        const auto second =
            decode_method_reply(call(*emu, connection, IO_CONNECT_METHOD,
                                     method_body({.selector = SELECTOR_SET_PURGEABLE, .scalars = {id, 1}, .scalar_output_max = 1})));
        EXPECT_EQ(second.scalars.at(0), 3u);
    }

    TEST(IoSurface, CreateMachPortHandsOutASendRightNamingTheSurface)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);

        const auto reply =
            decode_method_reply(call(*emu, connection, IO_CONNECT_METHOD,
                                     method_body({.selector = SELECTOR_CREATE_MACH_PORT, .scalars = {id, 0}, .scalar_output_max = 1})));

        ASSERT_EQ(reply.code, 0u);
        ASSERT_EQ(reply.scalars.size(), 1u);
        const auto object = emu->mach.ports.object_of(static_cast<port_name_t>(reply.scalars[0]));
        EXPECT_EQ(object.kind, kernel_object_kind::io_object);
        EXPECT_EQ(object.id & 0xFFFFFFFFull, id) << "the port names the surface";
        EXPECT_NE(object.id & 0x2000000000000000ull, 0u) << "and is tagged apart from the registry nodes and the connections";
    }

    TEST(IoSurface, AnUnmodelledSelectorIsRefusedRatherThanAnsweredEmpty)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);

        EXPECT_EQ(error_code_of(call(*emu, connection, IO_CONNECT_METHOD, method_body({.selector = 27}))), IO_RETURN_UNSUPPORTED);

        EXPECT_EQ(error_code_of(call(*emu, connection, IO_CONNECT_METHOD,
                                     method_body({.selector = SELECTOR_SET_PURGEABLE, .scalars = {9999, 0}, .scalar_output_max = 1}))),
                  IO_RETURN_BAD_ARGUMENT)
            << "a surface id nothing was ever created under names no surface";
    }

    TEST(IoSurface, LockAndUnlockMoveTheSeedThroughTrapOneHundred)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);
        const auto info = read_u64(created.inband, 0x08);

        emu->memory.allocate_memory(SCRATCH, sogen::MACOS_PAGE_SIZE, sogen::memory_permission::read_write);
        trap_runner run{*emu};

        EXPECT_EQ(run({connection, 2, id, 0, SCRATCH}), sogen::mach::kr::success);
        EXPECT_EQ(read_guest<uint32_t>(*emu, SCRATCH), 0u) << "a lock does not invalidate anybody";

        EXPECT_EQ(run({connection, 3, id, 0, SCRATCH}), sogen::mach::kr::success);
        EXPECT_EQ(read_guest<uint32_t>(*emu, SCRATCH), 1u) << "the unlock is what bumps the seed";
        EXPECT_EQ(read_guest<uint32_t>(*emu, info + 0x0C), 1u)
            << "IOSurfaceClientGetSeed reads the shared record rather than asking the kernel";

        EXPECT_EQ(run({connection, 2, id, 1, SCRATCH}), sogen::mach::kr::success);
        EXPECT_EQ(run({connection, 3, id, 1, SCRATCH}), sogen::mach::kr::success);
        EXPECT_EQ(read_guest<uint32_t>(*emu, info + 0x0C), 1u)
            << "a reader unlocking invalidates nobody, so kIOSurfaceLockReadOnly leaves the seed alone";
    }

    TEST(IoSurface, TheUseCountMovesInTheSharedRecord)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);
        const auto info = read_u64(created.inband, 0x08);

        trap_runner run{*emu};

        EXPECT_EQ(run({connection, 0, id, 0}), sogen::mach::kr::success);
        EXPECT_EQ(read_guest<uint64_t>(*emu, info + 0x18), 1u) << "IOSurfaceClientIsInUse reads the shared record at +0x18";

        EXPECT_EQ(run({connection, 1, id, 0}), sogen::mach::kr::success);
        EXPECT_EQ(read_guest<uint64_t>(*emu, info + 0x18), 0u);
    }

    TEST(IoSurface, ReleaseUnmapsTheSurfaceOnlyWhenTheLastReferenceGoes)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        const auto created = create_surface(*emu, connection, properties_of(4, 4, 16, 0x42475241, 4));
        ASSERT_EQ(created.code, 0u);
        const auto id = read_u32(created.inband, 0x18);
        const auto base = read_u64(created.inband, 0x00);

        trap_runner run{*emu};

        EXPECT_EQ(run({connection, 5, id}), sogen::mach::kr::success);
        EXPECT_EQ(run({connection, 4, id}), sogen::mach::kr::success);
        EXPECT_EQ(emu->ui.surfaces.live_count(), 1u) << "the retain is still holding it";

        EXPECT_EQ(run({connection, 4, id}), sogen::mach::kr::success);
        EXPECT_EQ(emu->ui.surfaces.live_count(), 0u);

        std::array<uint8_t, 4> scratch{};
        EXPECT_FALSE(emu->memory.try_read_memory(base, scratch.data(), scratch.size())) << "the pixels are gone with it";
    }

    TEST(IoSurface, AnUnmodelledTrapIndexIsRefused)
    {
        const auto emu = macos_test::make_emulator();
        const auto connection = open_surface_connection(*emu);
        trap_runner run{*emu};

        EXPECT_EQ(run({connection, 11, 1, 0}), IO_RETURN_UNSUPPORTED);
        EXPECT_EQ(run({emu->mach.io_master_port(), 2, 1, 0}), sogen::mach::kr::invalid_argument)
            << "a trap on a port that is not a user client names nothing to lock";
    }
}
