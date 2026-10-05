#include "../windows-emulator/ports/sspi_context.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    template <typename T>
    T read_value(const std::span<const uint8_t> bytes, const size_t offset)
    {
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    std::vector<uint8_t> read_capture()
    {
        const std::filesystem::path path =
            std::filesystem::path{SOGEN_SOURCE_DIR} / "sspi_port_handoff/windows_capture/capture/sspi/isc_0006_output_context.bin";
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return {};
        }
        const auto size = stream.tellg();
        if (size < 0)
        {
            return {};
        }
        std::vector<uint8_t> result(static_cast<size_t>(size));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(result.data()), size);
        return result;
    }

    struct item
    {
        uint32_t type{};
        uint32_t padded_size{};
        uint32_t reported_size{};
        std::span<const uint8_t> payload{};
    };

    std::vector<item> parse_items(const std::span<const uint8_t> bytes)
    {
        std::vector<item> result;
        size_t offset{};
        while (offset + 16 <= bytes.size())
        {
            item current{
                .type = read_value<uint32_t>(bytes, offset),
                .padded_size = read_value<uint32_t>(bytes, offset + 4),
                .reported_size = read_value<uint32_t>(bytes, offset + 8),
            };
            if (current.padded_size > bytes.size() - offset - 16)
            {
                return {};
            }
            current.payload = bytes.subspan(offset + 16, current.padded_size);
            result.push_back(current);
            offset += 16 + current.padded_size;
            if (current.type == 0)
            {
                break;
            }
        }
        return offset == bytes.size() ? result : std::vector<item>{};
    }

    sogen::sspi::provider_context_input fixture_input()
    {
        return {
            .protocol = 0x0303,
            .cipher_suite = 0xc02b,
            .inbound_raw_key = {0x0e, 0x83, 0x0f, 0x61, 0x15, 0x4b, 0x85, 0xf9, 0x16, 0xf4, 0x39, 0x79, 0xc5, 0xd1, 0xfd, 0xc2},
            .inbound_fixed_iv = {0x6a, 0xc8, 0xe2, 0xda},
            .inbound_sequence = 1,
            .outbound_raw_key = {0x43, 0xad, 0xea, 0x9e, 0x4a, 0x6b, 0xbf, 0x85, 0xa1, 0x7b, 0x0d, 0x97, 0xf1, 0x33, 0x2e, 0xac},
            .outbound_fixed_iv = {0xba, 0x79, 0x03, 0xb0},
            .outbound_sequence = 1,
            .serialized_context_flags = 0x0000000008008200,
        };
    }
}

TEST(sspi_context, builds_minimal_provider_context_from_named_inputs)
{
    const auto generated = sogen::sspi::build_provider_context(fixture_input());
    ASSERT_TRUE(generated);
    EXPECT_EQ(generated->size(), 1624);

    const auto items = parse_items(*generated);
    ASSERT_EQ(items.size(), 5);
    EXPECT_EQ(items[0].type, 1);
    EXPECT_EQ(items[0].padded_size, 200);
    EXPECT_EQ(items[0].reported_size, 0);
    EXPECT_EQ(items[1].type, 9);
    EXPECT_EQ(items[1].padded_size, 64);
    EXPECT_EQ(items[2].type, 2);
    EXPECT_EQ(items[2].padded_size, 640);
    EXPECT_EQ(items[3].type, 3);
    EXPECT_EQ(items[3].padded_size, 640);
    EXPECT_EQ(items[4].type, 0);

    const auto packed = items[0].payload;
    EXPECT_EQ(read_value<uint32_t>(packed, 0x00), 4);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x04), 1);
    EXPECT_EQ(read_value<uint64_t>(packed, 0x08), 0x0000000008008200);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x10), 0x800);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x14), 0xc02b);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x18), 0x17);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x1c), 0x100);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x20), 8);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x24), 8);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x2c), 1);
    EXPECT_EQ(read_value<uint64_t>(packed, 0x30), 1);
    EXPECT_EQ(read_value<uint64_t>(packed, 0x38), 1);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x50), 0);
    EXPECT_EQ(read_value<uint32_t>(packed, 0x98), 5);
    EXPECT_EQ(read_value<uint32_t>(packed, 0xb0), 0xffffffff);
}

TEST(sspi_context, key_records_match_native_capture_oracle)
{
    const auto capture = read_capture();
    ASSERT_EQ(capture.size(), 5096);
    const auto captured_items = parse_items(capture);
    ASSERT_EQ(captured_items.size(), 7);

    const auto generated = sogen::sspi::build_provider_context(fixture_input());
    ASSERT_TRUE(generated);
    const auto generated_items = parse_items(*generated);
    ASSERT_EQ(generated_items.size(), 5);

    EXPECT_TRUE(std::ranges::equal(generated_items[1].payload, captured_items[1].payload));
    EXPECT_TRUE(std::ranges::equal(generated_items[2].payload, captured_items[2].payload));
    EXPECT_TRUE(std::ranges::equal(generated_items[3].payload, captured_items[3].payload));
    EXPECT_EQ(read_value<uint32_t>(generated_items[2].payload, 0x10), 1);
    EXPECT_EQ(read_value<uint32_t>(generated_items[3].payload, 0x10), 0);
}

TEST(sspi_context, reports_exact_missing_tls_record_bytes)
{
    const std::array<uint8_t, 8> record = {22, 3, 3, 0, 3, 1, 2, 3};
    for (size_t size = 0; size < 5; ++size)
    {
        const auto framing = sogen::sspi::frame_tls_records(std::span{record}.first(size));
        EXPECT_EQ(framing.complete_size, 0);
        EXPECT_EQ(framing.missing_size, 5 - size);
    }

    const auto header_only = sogen::sspi::frame_tls_records(std::span{record}.first(5));
    EXPECT_EQ(header_only.complete_size, 0);
    EXPECT_EQ(header_only.missing_size, 3);
    const auto truncated_payload = sogen::sspi::frame_tls_records(std::span{record}.first(7));
    EXPECT_EQ(truncated_payload.complete_size, 0);
    EXPECT_EQ(truncated_payload.missing_size, 1);
}

TEST(sspi_context, frames_coalesced_records_before_partial_tail)
{
    const std::array<uint8_t, 14> records = {22, 3, 3, 0, 1, 0xaa, 20, 3, 3, 0, 0, 23, 3, 3};
    const auto framing = sogen::sspi::frame_tls_records(records);
    EXPECT_EQ(framing.complete_size, 11);
    EXPECT_EQ(framing.missing_size, 2);
}

TEST(sspi_context, counts_only_post_ccs_owned_records)
{
    const std::array<uint8_t, 5> handshake = {22, 3, 3, 0, 0};
    const std::array<uint8_t, 6> ccs = {20, 3, 3, 0, 1, 1};
    const std::array<uint8_t, 5> application = {23, 3, 3, 0, 0};
    sogen::sspi::tls_record_counter inbound;
    sogen::sspi::tls_record_counter outbound;

    ASSERT_TRUE(inbound.observe(handshake));
    ASSERT_TRUE(outbound.observe(handshake));
    EXPECT_EQ(inbound.value(), 0);
    EXPECT_EQ(outbound.value(), 0);
    ASSERT_TRUE(inbound.observe(ccs));
    ASSERT_TRUE(outbound.observe(ccs));
    EXPECT_EQ(inbound.value(), 0);
    EXPECT_EQ(outbound.value(), 0);
    ASSERT_TRUE(inbound.observe(handshake));
    ASSERT_TRUE(outbound.observe(handshake));
    EXPECT_EQ(inbound.value(), 1);
    EXPECT_EQ(outbound.value(), 1);

    std::array<uint8_t, 10> finished_and_extra{};
    std::memcpy(finished_and_extra.data(), handshake.data(), handshake.size());
    std::memcpy(finished_and_extra.data() + handshake.size(), application.data(), application.size());
    const auto framing = sogen::sspi::frame_tls_records(finished_and_extra);
    ASSERT_EQ(framing.complete_size, finished_and_extra.size());
    ASSERT_FALSE(inbound.observe(finished_and_extra));
    EXPECT_EQ(inbound.value(), 1);
}
