#include "std_include.hpp"

#include "../windows-emulator/ports/sspi_context.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace
{
    template <typename T>
    T read_value(const std::span<const uint8_t> bytes, const size_t offset)
    {
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
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

#ifdef _WIN32
namespace
{
    std::vector<std::vector<uint8_t>> host_root_certificates(const size_t count)
    {
        std::vector<std::vector<uint8_t>> certificates{};
        auto* store = CertOpenSystemStoreW(0, L"ROOT");
        if (!store)
        {
            return certificates;
        }

        for (const CERT_CONTEXT* certificate = nullptr; certificates.size() < count;)
        {
            certificate = CertEnumCertificatesInStore(store, certificate);
            if (!certificate)
            {
                break;
            }
            certificates.emplace_back(certificate->pbCertEncoded, certificate->pbCertEncoded + certificate->cbCertEncoded);
        }

        CertCloseStore(store, 0);
        return certificates;
    }
}

// The caller's crypt32 rebuilds the chain from the serialized store and the leaf from the serialized element.
TEST(sspi_context, serialized_certificate_context_is_readable_by_crypt32)
{
    const auto chain = host_root_certificates(2);
    ASSERT_EQ(2u, chain.size());
    const auto serialized = sogen::sspi::serialize_certificate_context(chain);

    const auto store_size = read_value<uint32_t>(serialized, 0);
    CRYPT_DATA_BLOB store_blob{.cbData = store_size, .pbData = const_cast<uint8_t*>(serialized.data() + sizeof(uint32_t))};
    auto* store = CertOpenStore(CERT_STORE_PROV_SERIALIZED, X509_ASN_ENCODING, 0, 0, &store_blob);
    ASSERT_NE(nullptr, store);

    std::vector<std::vector<uint8_t>> parsed{};
    for (const CERT_CONTEXT* certificate = nullptr; (certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr;)
    {
        parsed.emplace_back(certificate->pbCertEncoded, certificate->pbCertEncoded + certificate->cbCertEncoded);
    }
    CertCloseStore(store, 0);
    EXPECT_EQ(chain.size(), parsed.size());
    for (const auto& certificate : chain)
    {
        EXPECT_NE(parsed.end(), std::ranges::find(parsed, certificate));
    }

    const auto element_offset = sizeof(uint32_t) + store_size;
    const auto element_size = read_value<uint32_t>(serialized, element_offset);
    ASSERT_EQ(serialized.size(), element_offset + sizeof(uint32_t) + element_size);

    const void* context = nullptr;
    DWORD context_type = 0;
    ASSERT_TRUE(CertAddSerializedElementToStore(nullptr, serialized.data() + element_offset + sizeof(uint32_t), element_size,
                                                CERT_STORE_ADD_ALWAYS, 0, CERT_STORE_CERTIFICATE_CONTEXT_FLAG, &context_type, &context));
    const auto* leaf = static_cast<const CERT_CONTEXT*>(context);
    EXPECT_EQ(static_cast<DWORD>(CERT_STORE_CERTIFICATE_CONTEXT), context_type);
    EXPECT_EQ(chain.front(), std::vector<uint8_t>(leaf->pbCertEncoded, leaf->pbCertEncoded + leaf->cbCertEncoded));
    CertFreeCertificateContext(leaf);
}
#endif
