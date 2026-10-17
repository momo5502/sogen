#include <gtest/gtest.h>

#include <platform/crypt_protect_backend.hpp>
#include "crypt_backends/dpapi_primitives.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

namespace sogen::test
{
    namespace
    {
        std::array<uint8_t, k_dpapi_master_key_size> test_master_key()
        {
            std::array<uint8_t, k_dpapi_master_key_size> key{};
            uint8_t next = 1;
            for (auto& byte : key)
            {
                byte = next++;
            }
            return key;
        }

        crypt_protect_rng sequenced_rng(std::vector<std::vector<uint8_t>> chunks)
        {
            auto state = std::make_shared<std::pair<std::vector<std::vector<uint8_t>>, size_t>>(std::move(chunks), 0);
            return [state](std::span<uint8_t> out) {
                ASSERT_LT(state->second, state->first.size());
                const auto& chunk = state->first.at(state->second++);
                ASSERT_EQ(chunk.size(), out.size());
                std::memcpy(out.data(), chunk.data(), out.size());
            };
        }
    }

    TEST(DpapiPrimitives, Sha1Abc)
    {
        const std::array<uint8_t, 3> input{'a', 'b', 'c'};
        const auto digest = dpapi_primitives::sha1(input);
        const std::array<uint8_t, 20> expected{0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                               0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
        EXPECT_EQ(digest, expected);
    }

    TEST(DpapiPrimitives, Sha512Abc)
    {
        const std::array<uint8_t, 3> input{'a', 'b', 'c'};
        const auto digest = dpapi_primitives::sha512(input);
        const std::array<uint8_t, 64> expected{
            0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba, 0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
            0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2, 0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
            0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8, 0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
            0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e, 0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f,
        };
        EXPECT_EQ(digest, expected);
    }

    TEST(DpapiPrimitives, Aes256EcbNist)
    {
        const std::array<uint8_t, 32> key{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
        std::array<uint8_t, 16> block{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        std::array<uint32_t, 60> round_keys{};
        dpapi_primitives::expand_key_256(key.data(), round_keys.data());
        dpapi_primitives::aes256_encrypt_block(round_keys.data(), block.data());
        const std::array<uint8_t, 16> expected{0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
                                               0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
        EXPECT_EQ(block, expected);
    }

    TEST(CryptProtectBackend, RoundTripAndBlobLayout)
    {
        const auto key = test_master_key();
        std::array<uint8_t, 32> salt1{};
        std::array<uint8_t, 32> mac_salt{};
        salt1.fill(0x11);
        mac_salt.fill(0x22);
        auto backend = create_windows_dpapi_backend(
            {}, sequenced_rng({std::vector<uint8_t>(salt1.begin(), salt1.end()), std::vector<uint8_t>(mac_salt.begin(), mac_salt.end())}),
            key);

        const std::vector<uint8_t> plain{0x61, 0x62, 0x63};
        const std::vector<uint8_t> entropy{0x01, 0x02, 0x03, 0x04};
        crypt_protect_request request{};
        request.data = plain;
        request.entropy = entropy;
        request.description = u"sogen-test";

        const auto protected_blob = backend->protect(request);
        ASSERT_TRUE(protected_blob.ok);
        ASSERT_FALSE(protected_blob.data.empty());

        dpapi_blob_view view{};
        ASSERT_TRUE(parse_dpapi_blob(protected_blob.data, view));
        EXPECT_EQ(view.provider_guid, k_dpapi_default_provider_guid);
        EXPECT_EQ(view.master_key_guid, k_emulator_dpapi_master_key_guid);
        EXPECT_EQ(view.alg_crypt, k_calg_aes_256);
        EXPECT_EQ(view.alg_hash, k_calg_sha_512);
        EXPECT_EQ(view.crypt_bits, 256u);
        EXPECT_EQ(view.hash_bits, 512u);
        EXPECT_EQ(view.salt_len, 32u);
        EXPECT_EQ(view.mac_salt_len, 32u);
        EXPECT_EQ(view.sign_len, 64u);

        crypt_protect_request unprotect_request{};
        unprotect_request.data = protected_blob.data;
        unprotect_request.entropy = entropy;
        const auto unprotected = backend->unprotect(unprotect_request, true);
        ASSERT_TRUE(unprotected.ok);
        EXPECT_EQ(unprotected.data, plain);
        EXPECT_EQ(unprotected.description, u"sogen-test");

        crypt_protect_request wrong_entropy{};
        wrong_entropy.data = protected_blob.data;
        const auto failed = backend->unprotect(wrong_entropy, false);
        EXPECT_FALSE(failed.ok);
    }

    TEST(CryptProtectBackend, PinnedSaltIsDeterministic)
    {
        const auto key = test_master_key();
        std::array<uint8_t, 32> salt1{};
        std::array<uint8_t, 32> mac_salt{};
        salt1.fill(0x33);
        mac_salt.fill(0x44);
        const auto make_backend = [&] {
            return create_windows_dpapi_backend(
                {},
                sequenced_rng({std::vector<uint8_t>(salt1.begin(), salt1.end()), std::vector<uint8_t>(mac_salt.begin(), mac_salt.end())}),
                key);
        };

        const std::vector<uint8_t> plain{0x68, 0x69};
        crypt_protect_request request{};
        request.data = plain;
        const auto first = make_backend()->protect(request);
        const auto second = make_backend()->protect(request);
        ASSERT_TRUE(first.ok);
        ASSERT_TRUE(second.ok);
        EXPECT_EQ(first.data, second.data);
    }

    TEST(CryptProtectBackend, DefaultProtectIsDeterministic)
    {
        const std::vector<uint8_t> plain{0x61, 0x62, 0x63};
        crypt_protect_request request{};
        request.data = plain;
        request.description = u"sogen-test";

        const auto first = create_windows_dpapi_backend({})->protect(request);
        const auto second = create_windows_dpapi_backend({})->protect(request);
        ASSERT_TRUE(first.ok);
        ASSERT_TRUE(second.ok);
        EXPECT_EQ(first.data, second.data);
    }

    TEST(CryptProtectBackend, PersistsEmulatorMasterKey)
    {
        const auto root = std::filesystem::temp_directory_path() / "sogen-dpapi-test" / std::to_string(std::random_device{}());
        std::filesystem::create_directories(root);
        const auto key = test_master_key();
        const std::vector<uint8_t> plain{0x01, 0x02, 0x03};
        crypt_protect_request request{};
        request.data = plain;

        auto first = create_windows_dpapi_backend(root, {}, key);
        const auto protected_blob = first->protect(request);
        ASSERT_TRUE(protected_blob.ok);
        first.reset();

        auto second = create_windows_dpapi_backend(root);
        crypt_protect_request unprotect_request{};
        unprotect_request.data = protected_blob.data;
        const auto unprotected = second->unprotect(unprotect_request, false);
        ASSERT_TRUE(unprotected.ok);
        EXPECT_EQ(unprotected.data, plain);
        std::filesystem::remove_all(root);
    }

    TEST(CryptProtectBackend, EmptyPlaintextRoundTrip)
    {
        const auto key = test_master_key();
        auto backend = create_windows_dpapi_backend({}, {}, key);
        crypt_protect_request request{};
        const auto protected_blob = backend->protect(request);
        ASSERT_TRUE(protected_blob.ok);
        ASSERT_FALSE(protected_blob.data.empty());

        crypt_protect_request unprotect_request{};
        unprotect_request.data = protected_blob.data;
        const auto unprotected = backend->unprotect(unprotect_request, false);
        ASSERT_TRUE(unprotected.ok);
        EXPECT_TRUE(unprotected.data.empty());
    }
}
