#pragma once

#include "compiler.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sogen
{
    inline constexpr uint32_t k_calg_aes_256 = 0x6610;
    inline constexpr uint32_t k_calg_sha_512 = 0x800e;
    inline constexpr uint32_t k_cryptprotect_ui_forbidden = 0x1;
    inline constexpr uint32_t k_cryptprotect_local_machine = 0x4;
    inline constexpr uint32_t k_error_invalid_data = 13;
    inline constexpr uint32_t k_error_invalid_parameter = 87;

    // CRYPTPROTECT_DEFAULT_PROVIDER (MSDN). Packed little-endian as stored in DPAPI blobs.
    inline constexpr std::array<uint8_t, 16> k_dpapi_default_provider_guid{
        0xd0, 0x8c, 0x9d, 0xdf, 0x01, 0x15, 0xd1, 0x11, 0x8c, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x97, 0xeb,
    };

    // Emulator-owned master-key identifier. Not a Windows Protect-folder GUID.
    inline constexpr std::array<uint8_t, 16> k_emulator_dpapi_master_key_guid{
        0x65, 0x67, 0x6f, 0x73, 0x01, 0x6e, 0x00, 0x40, 0x80, 0x00, 0x44, 0x50, 0x41, 0x50, 0x49, 0x31,
    };

    inline constexpr size_t k_dpapi_master_key_size = 64;

    using crypt_protect_rng = std::function<void(std::span<uint8_t>)>;

    struct crypt_protect_request
    {
        std::span<const uint8_t> data{};
        std::span<const uint8_t> entropy{};
        std::u16string_view description{};
        uint32_t flags{};
    };

    struct crypt_protect_result
    {
        bool ok{};
        uint32_t last_error{};
        std::vector<uint8_t> data{};
        std::u16string description{};
    };

    struct dpapi_blob_view
    {
        uint32_t outer_version{};
        std::array<uint8_t, 16> provider_guid{};
        uint32_t inner_version{};
        std::array<uint8_t, 16> master_key_guid{};
        uint32_t flags{};
        uint32_t alg_crypt{};
        uint32_t crypt_bits{};
        uint32_t alg_hash{};
        uint32_t hash_bits{};
        uint32_t salt_len{};
        uint32_t mac_salt_len{};
        uint32_t hmac_key_len{};
        uint32_t data_len{};
        uint32_t sign_len{};
    };

    class crypt_protect_backend
    {
      public:
        virtual ~crypt_protect_backend() = default;

        virtual crypt_protect_result protect(const crypt_protect_request& request) = 0;
        virtual crypt_protect_result unprotect(const crypt_protect_request& request, bool return_description) = 0;
    };

    bool parse_dpapi_blob(std::span<const uint8_t> blob, dpapi_blob_view& view);

    std::unique_ptr<crypt_protect_backend> create_windows_dpapi_backend(const std::filesystem::path& emulation_root,
                                                                        crypt_protect_rng rng = {},
                                                                        std::span<const uint8_t> master_key = {});

    std::unique_ptr<crypt_protect_backend> create_default_crypt_protect_backend(const std::filesystem::path& emulation_root);
}
