#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sogen::ksecdd_data
{
    struct image_registration
    {
        std::u16string_view name;
        std::uint64_t type;
    };

    struct provider_registration
    {
        std::u16string_view name;
        image_registration user_mode;
        image_registration kernel_mode;
    };

    enum class provider_index : std::uint8_t
    {
        primitive,
        key_storage,
        ssl,
        key_protection,
        client_key_protection,
    };

    using enum provider_index;

    inline constexpr std::array<provider_registration, 5> providers = {{
        {.name = u"Microsoft Primitive Provider",
         .user_mode = {.name = u"bcryptprimitives.dll", .type = 1},
         .kernel_mode = {.name = u"cng.sys", .type = 1}},
        {.name = u"Microsoft Software Key Storage Provider", .user_mode = {.name = u"ncryptprov.dll", .type = 0x10000}, .kernel_mode = {}},
        {.name = u"Microsoft SSL Protocol Provider",
         .user_mode = {.name = u"ncryptsslp.dll", .type = 0},
         .kernel_mode = {.name = u"cng.sys", .type = 0}},
        {.name = u"Microsoft Key Protection Provider", .user_mode = {.name = u"mskeyprotect.dll", .type = 0x10000}, .kernel_mode = {}},
        {.name = u"Windows Client Key Protection Provider", .user_mode = {.name = u"mskeyprotcli.dll", .type = 0x10000}, .kernel_mode = {}},
    }};

    constexpr const provider_registration& provider_of(const provider_index index)
    {
        return providers[static_cast<std::size_t>(index)];
    }

    inline constexpr std::uint32_t marker_request_flags = 0xFFFFFFFE;

    struct function_registration
    {
        constexpr function_registration(const std::u16string_view function_name, const std::uint32_t function_interface,
                                        const provider_index function_provider, const std::uint32_t function_key_length = 0,
                                        const std::uint32_t function_listing_marker = 0, const bool function_kernel_mode = true)
            : name(function_name),
              interface_id(function_interface),
              provider(function_provider),
              key_length(function_key_length),
              listing_marker(function_listing_marker),
              kernel_mode(function_kernel_mode)
        {
        }

        std::u16string_view name;
        std::uint32_t interface_id;
        provider_index provider;
        std::uint32_t key_length;
        std::uint32_t listing_marker;
        bool kernel_mode;
    };

    inline constexpr std::array functions = std::to_array<function_registration>({
        {u"AES", 1, primitive, 128},
        {u"3DES", 1, primitive, 128},
        {u"3DES_112", 1, primitive, 112},
        {u"XTS-AES", 1, primitive},
        {u"DESX", 1, primitive, 128},
        {u"DES", 1, primitive, 128},
        {u"RC2", 1, primitive, 128},
        {u"RC4", 1, primitive, 128},
        {u"CHACHA20_POLY1305", 1, primitive},
        {u"SHA256", 2, primitive},
        {u"SHA384", 2, primitive},
        {u"SHA512", 2, primitive},
        {u"SHA3-256", 2, primitive},
        {u"SHA3-384", 2, primitive},
        {u"SHA3-512", 2, primitive},
        {u"CSHAKE128", 2, primitive},
        {u"CSHAKE256", 2, primitive},
        {u"KMAC128", 2, primitive},
        {u"KMAC256", 2, primitive},
        {u"SHAKE128", 2, primitive},
        {u"SHAKE256", 2, primitive},
        {u"SHA1", 2, primitive},
        {u"MD5", 2, primitive},
        {u"MD4", 2, primitive},
        {u"MD2", 2, primitive},
        {u"AES-GMAC", 2, primitive},
        {u"AES-CMAC", 2, primitive},
        {u"RSA", 3, primitive, 1024, 0xffffffff},
        {u"DH", 4, primitive, 1024},
        {u"ECDH_P256", 4, primitive, 256},
        {u"ECDH_P384", 4, primitive, 384},
        {u"ECDH_P521", 4, primitive, 521},
        {u"ECDH", 4, primitive},
        {u"RSA_SIGN", 5, primitive, 1024},
        {u"ECDSA_P256", 5, primitive, 256},
        {u"ECDSA_P384", 5, primitive, 384},
        {u"ECDSA_P521", 5, primitive, 521},
        {u"ECDSA", 5, primitive},
        {u"DSA", 5, primitive, 1024, 0, false},
        {u"ML-DSA", 5, primitive},
        {u"Composite-ML-DSA", 5, primitive},
        {u"RNG", 6, primitive, 0, marker_request_flags},
        {u"FIPS186DSARNG", 6, primitive},
        {u"DUALECRNG", 6, primitive},
        {u"SP800_108_CTR_HMAC", 7, primitive},
        {u"SP800_56A_CONCAT", 7, primitive},
        {u"PBKDF2", 7, primitive},
        {u"CAPI_KDF", 7, primitive},
        {u"TLS1_1_KDF", 7, primitive},
        {u"TLS1_2_KDF", 7, primitive},
        {u"HKDF", 7, primitive},
        {u"ML-KEM", 8, primitive, 0, 0xffffffff},
        {u"Composite-ML-KEM", 8, primitive},
        {u"KEY_STORAGE", 0x10001, key_storage, 0, 0xFFFFFFFF, false},
        {u"TLS_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA", 0x10002, ssl},
        {u"TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA", 0x10002, ssl},
        {u"TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_256_CBC_SHA256", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_128_CBC_SHA256", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_256_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_AES_128_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_NULL_SHA256", 0x10002, ssl},
        {u"TLS_RSA_WITH_NULL_SHA", 0x10002, ssl},
        {u"TLS_PSK_WITH_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_PSK_WITH_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_PSK_WITH_AES_256_CBC_SHA384", 0x10002, ssl},
        {u"TLS_PSK_WITH_AES_128_CBC_SHA256", 0x10002, ssl},
        {u"TLS_PSK_WITH_NULL_SHA384", 0x10002, ssl},
        {u"TLS_PSK_WITH_NULL_SHA256", 0x10002, ssl},
        {u"TLS_CHACHA20_POLY1305_SHA256", 0x10002, ssl},
        {u"TLS_DHE_RSA_WITH_AES_256_GCM_SHA384", 0x10002, ssl},
        {u"TLS_DHE_RSA_WITH_AES_128_GCM_SHA256", 0x10002, ssl},
        {u"TLS_DHE_RSA_WITH_AES_256_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_RSA_WITH_AES_128_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_3DES_EDE_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_AES_256_CBC_SHA256", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_AES_128_CBC_SHA256", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_AES_256_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_AES_128_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_3DES_EDE_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_RC4_128_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_RC4_128_MD5", 0x10002, ssl},
        {u"TLS_RSA_WITH_DES_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_DSS_WITH_DES_CBC_SHA", 0x10002, ssl},
        {u"TLS_DHE_DSS_EXPORT1024_WITH_DES_CBC_SHA", 0x10002, ssl},
        {u"TLS_RSA_WITH_NULL_MD5", 0x10002, ssl},
        {u"TLS_RSA_EXPORT1024_WITH_RC4_56_SHA", 0x10002, ssl},
        {u"TLS_RSA_EXPORT_WITH_RC4_40_MD5", 0x10002, ssl},
        {u"TLS_RSA_EXPORT1024_WITH_DES_CBC_SHA", 0x10002, ssl},
        {u"SID", 0x10004, key_protection},
        {u"SDDL", 0x10004, key_protection},
        {u"LOCAL", 0x10004, key_protection},
        {u"WEBCREDENTIALS", 0x10004, key_protection},
        {u"MSIDCRL", 0x10004, client_key_protection},
        {u"DRACERTIFICATE", 0x10004, key_protection},
        {u"CERTIFICATE", 0x10004, key_protection},
        {u"VAULTCREDENTIALS", 0x10004, key_protection},
        {u"KEYFILE", 0x10004, key_protection},
        {u"LOCKEDCREDENTIALS", 0x10004, client_key_protection},
    });

    struct interface_default
    {
        constexpr interface_default(const std::uint32_t default_interface, const std::u16string_view default_function)
            : interface_id(default_interface),
              function(default_function)
        {
        }

        std::uint32_t interface_id;
        std::u16string_view function;
    };

    inline constexpr std::array<interface_default, 12> interface_defaults = {{
        {1, u"AES"},
        {2, u"SHA256"},
        {3, u"RSA"},
        {4, u"DH"},
        {5, u"RSA_SIGN"},
        {6, u"RNG"},
        {7, u"SP800_108_CTR_HMAC"},
        {8, u"ML-KEM"},
        {0x10001, u"KEY_STORAGE"},
        {0x10002, u"TLS_AES_256_GCM_SHA384"},
        {0x10003, u""},
        {0x10004, u"SID"},
    }};
}
